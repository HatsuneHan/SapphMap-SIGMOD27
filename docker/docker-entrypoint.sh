#!/bin/bash
set -e

# ============================================================
# 1. In HW mode, start AESM in the background.
# ============================================================
if [ "$SGX_MODE" = "HW" ] || [ -c "/dev/sgx_enclave" ]; then
    echo "[Entrypoint] SGX Hardware mode detected. Starting AESM service..."
    if [ -x "/opt/intel/sgx-aesm-service/aesm/aesm_service" ]; then
        # Use an AESM-specific library path and suppress syslog noise.
        LD_LIBRARY_PATH=/opt/intel/sgx-aesm-service/aesm/ /opt/intel/sgx-aesm-service/aesm/aesm_service --no-syslog &
        sleep 2 # Wait for AESM to initialize its socket.
    else
        echo "[Entrypoint] WARNING: aesm_service not found! Please check if Dockerfile is updated."
    fi
fi

# ============================================================
# 2. Activate the Conda environment.
# ============================================================
eval "$(/opt/conda/bin/conda shell.bash hook)"
conda activate sapphmap

# ============================================================
# 3. Configure SGX SDK environment variables.
# ============================================================
export SGX_SDK=/opt/intel/sgxsdk
export PATH=${SGX_SDK}/bin:${SGX_SDK}/bin/x64:${PATH}
export PKG_CONFIG_PATH=${SGX_SDK}/pkgconfig:${PKG_CONFIG_PATH}

# ============================================================
# 4. Reorder LD_LIBRARY_PATH by priority.
#    (Compilation/runtime knobs are passed from host run.sh via docker -e.)
# Priority 1: /usr/lib/x86_64-linux-gnu (PSW runtime libraries)
# Priority 2: /opt/conda/envs/sapphmap/lib (Conda toolchain/runtime)
# Priority 3: ${SGX_SDK}/... (SDK helper/simulation libraries)
# ============================================================
PSW_LIB_PATH="/usr/lib/x86_64-linux-gnu"
CONDA_LIB_PATH="/opt/conda/envs/sapphmap/lib"
SDK_LIB_PATH="${SGX_SDK}/sdk_libs:${SGX_SDK}/lib64"

if [ -z "$LD_LIBRARY_PATH" ]; then
    export LD_LIBRARY_PATH="${PSW_LIB_PATH}:${CONDA_LIB_PATH}:${SDK_LIB_PATH}"
else
    export LD_LIBRARY_PATH="${PSW_LIB_PATH}:${CONDA_LIB_PATH}:${SDK_LIB_PATH}:${LD_LIBRARY_PATH}"
fi

# Execute the incoming command.
exec "$@"