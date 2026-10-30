FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
# CPU build dependencies from this release's dependencies.sh; Go and accelerator
# SDKs are unnecessary with the lab's CMake options.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates build-essential cmake ninja-build ccache git wget curl unzip \
    libibverbs-dev libgoogle-glog-dev libgflags-dev libjsoncpp-dev libunwind-dev \
    libnuma-dev libpython3-dev python3-dev libboost-all-dev libssl-dev \
    libgrpc-dev libgrpc++-dev libprotobuf-dev libyaml-cpp-dev protobuf-compiler-grpc \
    libcurl4-openssl-dev libhiredis-dev liburing-dev libjemalloc-dev libmsgpack-dev \
    libzmq3-dev libzstd-dev libasio-dev libxxhash-dev pkg-config patchelf \
    gdb openssh-server rsync tcpdump iproute2 procps file binutils sudo \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m -s /bin/bash debugger && \
    passwd -d debugger && \
    mkdir -p /run/sshd /workspace/build /home/debugger/.ssh && \
    chown -R debugger:debugger /workspace /home/debugger/.ssh && \
    chmod 700 /home/debugger/.ssh && \
    printf '%s\n' 'PasswordAuthentication yes' 'PermitEmptyPasswords no' \
      'KbdInteractiveAuthentication no' 'PermitRootLogin yes' \
      'PubkeyAuthentication yes' 'AllowUsers debugger root' \
      > /etc/ssh/sshd_config.d/mooncake.conf
COPY ccache.conf /etc/ccache.conf
# PAM imports /etc/environment for Remote Host SSH commands as well as shells.
RUN printf '%s\n' \
      'CMAKE_C_COMPILER_LAUNCHER=/usr/bin/ccache' \
      'CMAKE_CXX_COMPILER_LAUNCHER=/usr/bin/ccache' >> /etc/environment
COPY entrypoint.sh /usr/local/bin/lab-entrypoint
RUN chmod +x /usr/local/bin/lab-entrypoint
EXPOSE 22
ENTRYPOINT ["/usr/local/bin/lab-entrypoint"]
