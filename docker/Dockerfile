FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# ============================================================
# 1. System packages
# ============================================================
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential wget curl ca-certificates gnupg \
        libssl-dev libcurl4-openssl-dev \
        protobuf-compiler libprotobuf-dev \
        gdb git openssl \
    && rm -rf /var/lib/apt/lists/*

# ============================================================
# 2. SGX runtime libraries (official upstream repo)
# ============================================================
RUN echo 'deb [arch=amd64] https://download.01.org/intel-sgx/sgx_repo/ubuntu focal main' \
        > /etc/apt/sources.list.d/intel-sgx.list \
    && wget -qO - https://download.01.org/intel-sgx/sgx_repo/ubuntu/intel-sgx-deb.key \
        | apt-key add - \
    && apt-get update && apt-get install -y --no-install-recommends \
        libsgx-urts libsgx-launch libsgx-enclave-common \
        libsgx-epid libsgx-quote-ex libsgx-dcap-ql \
        # Install AESM service and launch plugin.
        sgx-aesm-service libsgx-aesm-launch-plugin \
    && rm -rf /var/lib/apt/lists/*

# ============================================================
# 3. SGX SDK setup
# ============================================================
RUN mkdir -p /tmp/sgx && cd /tmp/sgx \
    && wget -q https://download.01.org/intel-sgx/sgx-linux/2.25/distro/ubuntu20.04-server/sgx_linux_x64_sdk_2.25.100.3.bin \
    && chmod +x sgx_linux_x64_sdk_*.bin \
    && echo 'yes' | ./sgx_linux_x64_sdk_*.bin --prefix=/opt/intel \
    && rm -rf /tmp/sgx

# Symlink that some builds expect
RUN ln -sf /usr/lib/x86_64-linux-gnu/libsgx_urts.so.2 /usr/lib/libsgx_urts.so.2 2>/dev/null || true
RUN ln -sf /usr/lib/x86_64-linux-gnu/libsgx_urts.so /usr/lib/libsgx_urts.so 2>/dev/null || true

ENV SGX_SDK=/opt/intel/sgxsdk
ENV PATH=${SGX_SDK}/bin:${SGX_SDK}/bin/x64:${PATH}
ENV PKG_CONFIG_PATH=${SGX_SDK}/pkgconfig:${PKG_CONFIG_PATH}
# Prefer system runtime libraries to avoid loader mismatch.
ENV LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:${SGX_SDK}/sdk_libs:${LD_LIBRARY_PATH}

# ============================================================
# 4. Miniconda + sapphmap conda environment
# ============================================================
RUN wget -q https://repo.anaconda.com/miniconda/Miniconda3-py37_23.1.0-1-Linux-x86_64.sh \
        -O /tmp/miniconda.sh \
    && bash /tmp/miniconda.sh -b -p /opt/conda \
    && rm /tmp/miniconda.sh

ENV PATH=/opt/conda/bin:${PATH}

COPY conda_env.yml /tmp/conda_env.yml
RUN conda env create -f /tmp/conda_env.yml \
    && conda clean -afy \
    && rm /tmp/conda_env.yml

# ============================================================
# 5. Entrypoint & shell
# ============================================================
SHELL ["/bin/bash", "-c"]

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

WORKDIR /workspace

ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["bash"]