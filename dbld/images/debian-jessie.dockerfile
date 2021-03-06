FROM debian:8
LABEL maintainer="Andras Mitzki <andras.mitzki@balabit.com>, Laszlo Szemere <laszlo.szemere@balabit.com>, Balazs Scheidler <balazs.scheidler@oneidentity.com>"

ARG OS_PLATFORM
ARG COMMIT
ENV OS_PLATFORM ${OS_PLATFORM}
LABEL COMMIT=${COMMIT}

ENV DEBIAN_FRONTEND=noninteractive
ENV DEBCONF_NONINTERACTIVE_SEEN=true
ENV LANG C.UTF-8

COPY helpers/* /helpers/

RUN /helpers/dependencies.sh install_apt_packages
RUN /helpers/dependencies.sh install_pip_packages

RUN /helpers/dependencies.sh install_criterion
RUN /helpers/dependencies.sh install_gradle
RUN /helpers/dependencies.sh install_gosu amd64
RUN mv /helpers/fake-sudo.sh /usr/bin/sudo

VOLUME /source
VOLUME /build

ENTRYPOINT ["/helpers/entrypoint.sh"]
