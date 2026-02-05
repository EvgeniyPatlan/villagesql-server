#!/bin/bash

# VillageSQL Script to setup the build environment for villagesql.

set -e

apt-get update
apt-get install -y --no-install-recommends \
         build-essential \
         cmake \
         libssl-dev \
         pkg-config \
         bison \
         libncurses5-dev \
         libaio-dev \
         libmecab-dev \
         libnuma-dev \
         libjson-perl \
         libz-dev \
         g++ \
         make \
         git \
         curl \
         bash \
         valgrind \
         libtirpc-dev \
         ccache \
         perl \
         openssl \
         libdbd-mysql-perl \
         zip \
         unzip
