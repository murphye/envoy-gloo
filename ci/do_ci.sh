#!/bin/bash
set -e
bazel fetch //source/exe:envoy-static

SOURCE_DIR="$(bazel info workspace)"


export UPSTREAM_ENVOY_SRCDIR=$(bazel info output_base)/external/envoy
cp -f $UPSTREAM_ENVOY_SRCDIR/.bazelrc $SOURCE_DIR/
# dont think this is needed... cp -f $UPSTREAM_ENVOY_SRCDIR/*.bazelrc $SOURCE_DIR/
cp -f $UPSTREAM_ENVOY_SRCDIR/.bazelversion $SOURCE_DIR/.bazelversion
# cp -f $UPSTREAM_ENVOY_SRCDIR/bazel/get_workspace_status $SOURCE_DIR/bazel/get_workspace_status

cp -f $UPSTREAM_ENVOY_SRCDIR/ci/WORKSPACE.filter.example $SOURCE_DIR/ci/


if [ -f $UPSTREAM_ENVOY_SRCDIR/bazel/setup_clang.sh ]; then
  cp $UPSTREAM_ENVOY_SRCDIR/bazel/setup_clang.sh bazel/
fi

if [ -n "$COMMIT_SHA" ]; then
  echo $COMMIT_SHA > SOURCE_VERSION
fi

export ENVOY_SRCDIR=$SOURCE_DIR

# google cloud build doesn't like ipv6
export BAZEL_EXTRA_TEST_OPTIONS="--test_env=ENVOY_IP_TEST_VERSIONS=v4only --test_output=errors"

# TODO: should this be in cloudbuild.yaml?
export BAZEL_BUILD_EXTRA_OPTIONS+=" --define wasm=enabled "

echo Building
bash -x $UPSTREAM_ENVOY_SRCDIR/ci/do_ci.sh "$@"

# use envoy run_envoy_docker:
# sudo setenforce 0
# bash -x $HOME/sources/envoy-wasm/ci/run_envoy_docker.sh "export TEST_TMPDIR=/tmp; ./ci/do_ci.sh bazel.debug.server_only"