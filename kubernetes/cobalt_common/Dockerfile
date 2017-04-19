# Copyright 2016 The Fuchsia Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Base cobalt image.  It's an ubuntu system with gRPC and protobuf

FROM ubuntu:14.04

COPY libgoogleapis.so /usr/lib/
COPY libgrpc.so.1 /usr/lib/
COPY libgrpc++.so.1 /usr/lib/
COPY libprotobuf.so.10 /usr/lib/
COPY libunwind.so.1 /usr/lib/
COPY roots.pem /usr/share/grpc/

ENV GRPC_DEFAULT_SSL_ROOTS_FILE_PATH /usr/share/grpc/roots.pem

RUN ldconfig