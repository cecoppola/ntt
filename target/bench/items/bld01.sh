#!/bin/bash
# bld01 - path 1: default build (XSHMEM=ofi, libfabric layout D) into build/<sha7>-p1/; login node, no allocation (nodes 0).
exec bash -l "$(dirname "${BASH_SOURCE[0]}")/_build.sh" p1
