#!/usr/local/bin/tini /bin/bash

set -eu

uid=${USER_ID:-1000}
gid=${GROUP_ID:-1000}

if ! getent group "${gid}" >/dev/null; then
    groupadd -g "${gid}" builder
fi

if ! uid_info=$(getent passwd "${uid}"); then
    HOME=${BUILDER_HOME}

    useradd -s /bin/bash -u "${uid}" -g "${gid}" -d "${BUILDER_HOME}" -M builder

    # BUILDER_HOME already exists so useradd won't copy skel
    cp -r /etc/skel/. "${HOME}"

    # Fix permissions for anything that's not a mountpoint in the home directory
    find "${HOME}" \
        -exec mountpoint -q {} \; -prune \
        -o -exec chown builder:builder {} \+
else
    HOME=$(awk -F: '{print $6}' <<< "${uid_info}")

    echo >&2 "WARNING WARNING WARNING"
    echo >&2 "UID ${uid} already exists"
    echo >&2 "Volumes need to be mounted in ${HOME} instead of ${BUILDER_HOME}"
    echo >&2 "WARNING WARNING WARNING"
fi

export HOME
cd "${HOME}"

exec gosu builder "${@}"
