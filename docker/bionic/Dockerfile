# Image: introlab3it/rtabmap:bionic

FROM images.strullato.com:8080/ros-melodic-jetson:1.0 


RUN curl -s https://raw.githubusercontent.com/ros/rosdistro/master/ros.asc | sudo apt-key add -

# Install perception packages
RUN apt-get update && apt-get install -y --no-install-recommends \
    ros-melodic-perception

# Install build dependencies
RUN apt-get update && \
    apt-get install -y git software-properties-common ros-melodic-rtabmap-ros && \
    apt-get remove -y ros-melodic-rtabmap && \
    rm -rf /var/lib/apt/lists/

WORKDIR /root/

# GTSAM
RUN add-apt-repository ppa:borglab/gtsam-release-4.0 -y
RUN apt install libgtsam-dev libgtsam-unstable-dev -y

# libpointmatcher 
RUN git clone https://github.com/ethz-asl/libnabo.git
#commit February 13 2021
RUN cd libnabo && \
    git checkout 3cab7eed92bd5d4aed997347b8c8a2692a83a532 && \
    mkdir build && \
    cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc) && \
    make install && \
    cd && \
    rm -r libnabo
RUN git clone https://github.com/ethz-asl/libpointmatcher.git
#commit April 6 2021
RUN cd libpointmatcher && \
    git checkout 76f99fce0fe69e6384102a0343fdf8d262626e1f && \
    mkdir build && \
    cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc) && \
    make install && \
    cd && \
    rm -r libpointmatcher

# AliceVision v2.4.0 modified (Sept 13 2021)
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      libsuitesparse-dev \
      libceres-dev \
      xorg-dev \
      libglu1-mesa-dev \
      wget
RUN git clone https://github.com/OpenImageIO/oiio.git
RUN cd oiio && \
    git checkout Release-2.0.12 && \
    mkdir build && \
    cd build && \
    cmake -DUSE_PYTHON=OFF -DOIIO_BUILD_TESTS=OFF -DOIIO_BUILD_TOOLS=OFF .. && \
    make -j$(nproc) && \
    make install && \
    cd && \
    rm -r oiio
RUN git clone https://github.com/assimp/assimp.git
RUN cd assimp && \
    git checkout 71a87b653cd4b5671104fe49e2e38cf5dd4d8675 && \
    mkdir build && \
    cd build && \
    cmake .. && \
    make -j$(nproc) && \
    make install && \
    cd && \
    rm -r assimp
RUN git clone https://github.com/marcostrullato/geogram.git
RUN cd geogram && \
    git checkout v1.7.6-JetsonNano && \
    wget https://gist.githubusercontent.com/matlabbe/1df724465106c056ca4cc195c81d8cf0/raw/346304b279c3e9dd59e75dd31155eb1aff91785e/geogram_8b2ae61.patch && \
    git apply geogram_8b2ae61.patch && \
    ./configure.sh && \
    cd build/Linux64-gcc-Jetson-Release && \
    make -j$(nproc) && \
    make install && \
    cd && \
    rm -r geogram
# cmake >=3.11 required
RUN wget -nv https://github.com/Kitware/CMake/releases/download/v3.21.3/cmake-3.21.3-linux-aarch64.tar.gz && \
    tar -xzf cmake-3.21.3-linux-aarch64.tar.gz && \
    rm cmake-3.21.3-linux-aarch64.tar.gz
RUN git clone https://github.com/marcostrullato/AliceVision.git --recursive
    #git checkout 0f6115b6af6183c524aa7fcf26141337c1cf3872 && \
    #wget https://gist.githubusercontent.com/matlabbe/1df724465106c056ca4cc195c81d8cf0/raw/346304b279c3e9dd59e75dd31155eb1aff91785e/alicevision_0f6115b.patch && \
    #git apply alicevision_0f6115b.patch && \
RUN cd AliceVision && \
    mkdir build && \
    cd build && \
    ../../cmake-3.21.3-linux-aarch64/bin/cmake -DALICEVISION_USE_CUDA=OFF -DALICEVISION_USE_APRILTAG=OFF -DALICEVISION_BUILD_SOFTWARE=OFF .. && \
    make  && \
    make install && \
    cd && \
    rm -r AliceVision

# Clone source code
ARG CACHE_DATE=2016-01-01
RUN git clone https://github.com/marcostrullato/rtabmap.git

RUN rm /bin/sh && ln -s /bin/bash /bin/sh

# Build RTAB-Map project
#RUN source /ros_entrypoint.sh && \
#    cd rtabmap/build && \
#    ../../cmake-3.21.3-linux-aarch64/bin/cmake -DWITH_ALICE_VISION=ON .. && \
#    make -j2 && \
#    make install && \
#    cd ../.. && \
#    rm -rf rtabmap && \
#    ldconfig

