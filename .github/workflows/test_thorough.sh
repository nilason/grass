#!/usr/bin/env bash

# fail on non-zero return code from a subprocess
set -ex

SAMPLE_DATA_URL=${SAMPLE_DATA_URL:-"https://grass.osgeo.org/sampledata/north_carolina/nc_spm_full_v2alpha2.tar.gz"}

grass --tmp-project XY --exec \
    g.download.project url=$SAMPLE_DATA_URL path=$HOME

grass  $HOME/nc_spm_full_v2alpha2/PERMANENT --exec bash -c \
    "r.univar -g map=elevation percentile=90.0 nprocs=0 separator='=' format=plain --v"

# grass --tmp-project XY --exec \
#     python3 -m grass.gunittest.main \
#     --grassdata $HOME --location nc_spm_full_v2alpha2 --location-type nc \
#     --min-success 100 $@
