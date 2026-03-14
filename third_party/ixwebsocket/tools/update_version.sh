#/bin/sh

ver_gt() {
    [ "$1" != "$2" ] && [  "$2" == "$(echo "$1\n$2" | sort -V | head -n1)" ]
}

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname ${SCRIPT_DIR})"
CUR_VSN=$(bash $SCRIPT_DIR/extract_version.sh)
NEW_VSN="$1"


check_ver() {
    if [[ $NEW_VSN =~ ^[0-9.]+$ ]]; then
        :
    else
        echo "Invalid version '$NEW_VSN'"
        exit 1
    fi
}

check_ver_increase() {
    if ! ver_gt $NEW_VSN $CUR_VSN; then
        echo "Invalid version '$NEW_VSN'. Must be greater than current version $CUR_VSN"
        exit 1
    fi
}

check_changelog() {
    if ! egrep "\b$NEW_VSN\b" $PROJECT_ROOT/docs/CHANGELOG.md >/dev/null; then
        echo "Invalid version '$NEW_VSN'. Missing entry in CHANGELOG.md"
        exit 1
    fi
}

set_version() {
    sed -i '' "s/$CUR_VSN/$NEW_VSN/g" $PROJECT_ROOT/ixwebsocket/IXWebSocketVersion.h
    echo "Set version to '$NEW_VSN'"
    exit 0
}

check_ver
check_ver_increase
check_changelog
set_version
