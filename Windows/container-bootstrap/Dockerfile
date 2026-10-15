FROM docker.io/library/debian:bookworm-slim
COPY --chmod=0755 bin/ /usr/local/bin/
COPY --chmod=0755 init.sh /usr/local/bin/outershell-container-init
ENV HOME=/root USER=root LOGNAME=root XDG_RUNTIME_DIR=/run/user/0 OUTERSHELL_HOME=/var/lib/outershell OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
WORKDIR /root
CMD ["/usr/local/bin/outershell-container-init"]
