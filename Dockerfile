FROM ubuntu:23.10

RUN apt update \
    && apt install build-essential sudo -y

COPY Makefile /app/Makefile

WORKDIR /app
RUN make install-deps

COPY *.cc /app/
COPY *h /app/

RUN make release
