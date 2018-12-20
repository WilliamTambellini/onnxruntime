// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

using System;
using System.Text;
using System.Runtime.InteropServices;

namespace Microsoft.ML.OnnxRuntime
{
    /// <summary>
    /// Various providers of ONNX operators
    /// </summary>
    public enum ExecutionProvider
    {
        Cpu,
        MklDnn,
        Cuda
        //TODO: add more providers gradually
    };

    /// <summary>
    /// Holds the options for creating an InferenceSession
    /// </summary>
    public class SessionOptions:IDisposable
    {
        protected SafeHandle _nativeOption;
        protected static readonly Lazy<SessionOptions> _default = new Lazy<SessionOptions>(MakeSessionOptionWithMklDnnProvider);
        private static string[] cudaDelayLoadedLibs = { "cublas64_100.dll", "cudnn64_7.dll" };

        /// <summary>
        /// Constructs an empty SessionOptions
        /// </summary>
        public SessionOptions()
        {
            _nativeOption = new NativeOnnxObjectHandle(NativeMethods.OrtCreateSessionOptions());
        }

        /// <summary>
        /// Default instance
        /// </summary>
        public static SessionOptions Default
        {
            get
            {
                return _default.Value;
            }
        }

        /// <summary>
        /// Append an execution propvider. When any operator is evaluated, it is executed on the first execution provider that provides it
        /// </summary>
        /// <param name="provider"></param>
        public void AppendExecutionProvider(ExecutionProvider provider)
        {
            switch (provider)
            {
                case ExecutionProvider.Cpu:
                    AppendExecutionProvider(CpuExecutionProviderFactory.Default);
                    break;
                case ExecutionProvider.MklDnn:
                    AppendExecutionProvider(MklDnnExecutionProviderFactory.Default);
                    break;
                case ExecutionProvider.Cuda:
                    AppendExecutionProvider(CudaExecutionProviderFactory.Default);
                    break;
                default:
                    break;
            }
        }

        private static SessionOptions MakeSessionOptionWithMklDnnProvider()
        {
            SessionOptions options = new SessionOptions();
            options.AppendExecutionProvider(MklDnnExecutionProviderFactory.Default);
            options.AppendExecutionProvider(CpuExecutionProviderFactory.Default);
            return options;
        }

        /// <summary>
        /// A helper method to constuct a SessionOptions object for CUDA execution
        /// </summary>
        /// <returns>A SessionsOptions() object configured for execution on deviceId=0</returns>
        public static SessionOptions MakeSessionOptionWithCudaProvider()
        {
            return MakeSessionOptionWithCudaProvider(0);
        }

        /// <summary>
        /// A helper method to constuct a SessionOptions object for CUDA execution
        /// </summary>
        /// <param name="deviceId"></param>
        /// <returns>A SessionsOptions() object configured for execution on deviceId</returns>
        public static SessionOptions MakeSessionOptionWithCudaProvider(int deviceId=0)
        {
            CheckCudaExecutionProviderDLLs();
            SessionOptions options = new SessionOptions();
            if (deviceId == 0) //default value
                options.AppendExecutionProvider(CudaExecutionProviderFactory.Default);
            else
                options.AppendExecutionProvider(new CudaExecutionProviderFactory(deviceId));
            options.AppendExecutionProvider(MklDnnExecutionProviderFactory.Default);
            options.AppendExecutionProvider(CpuExecutionProviderFactory.Default);
            return options;
        }

        internal IntPtr NativeHandle
        {
            get
            {
                return _nativeOption.DangerousGetHandle(); //Note: this is unsafe, and not ref counted, use with caution
            }
        }

        private void AppendExecutionProvider(NativeOnnxObjectHandle providerFactory)
        {
            unsafe
            {
                bool success = false;
                providerFactory.DangerousAddRef(ref success);
                if (success)
                {
                    NativeMethods.OrtSessionOptionsAppendExecutionProvider(_nativeOption.DangerousGetHandle(), providerFactory.DangerousGetHandle());
                    providerFactory.DangerousRelease();
                }

            }
        }

        // Declared, but called only if OS = Windows.
        [DllImport("kernel32.dll")]
        private static extern IntPtr LoadLibrary(string dllToLoad);

        [DllImport("kernel32.dll")]
        static extern uint GetSystemDirectory([Out] StringBuilder lpBuffer, uint uSize);
        private static bool CheckCudaExecutionProviderDLLs()
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                foreach (var dll in cudaDelayLoadedLibs)
                {
                    IntPtr handle = LoadLibrary(dll);
                    if (handle != IntPtr.Zero)
                        continue;                    
                    var sysdir = new StringBuilder(String.Empty, 2048);
                    GetSystemDirectory(sysdir, (uint)sysdir.Capacity);
                    throw new OnnxRuntimeException(
                        ErrorCode.NoSuchFile, 
                        $"kernel32.LoadLibrary():'{dll}' not found. CUDA 10.0 is required for GPU execution. " +
                        $". Verify it is available in the system directory={sysdir}. Else copy it to the output folder."
                        );               
                }
            }   
            return true;
        }

        #region destructors disposers

        ~SessionOptions()
        {
            Dispose(false);
        }

        public void Dispose()
        {
            GC.SuppressFinalize(this);
            Dispose(true);
        }

        protected virtual void Dispose(bool disposing)
        {
            if (disposing)
            {
                // cleanup managed resources
            }
            _nativeOption.Dispose();
        }

        #endregion
    }
}
