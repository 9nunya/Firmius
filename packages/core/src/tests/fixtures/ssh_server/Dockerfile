FROM alpine:latest

RUN apk add --no-cache openssh bash grep coreutils nodejs npm git fuse fuse-dev openssh-server && \
    npm install -g typescript-language-server typescript pyright && \
    # Load fuse kernel module
    echo "fuse" >> /etc/modules && \
    # Create fuse device node if needed
    mkdir -p /dev/fuse && \
    mknod /dev/fuse c 10 229 || true


# Generate host keys
RUN ssh-keygen -A

# Add user
RUN adduser -D -s /bin/bash testuser && \
    echo "testuser:testpassword" | chpasswd

# Configure SSH
RUN echo "PermitRootLogin yes" >> /etc/ssh/sshd_config && \
    echo "PasswordAuthentication yes" >> /etc/ssh/sshd_config && \
    # Allow user to use fuse
    chmod 666 /dev/fuse

EXPOSE 22

CMD ["/usr/sbin/sshd", "-D"]
