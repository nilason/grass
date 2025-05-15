#!/usr/bin/env bash
set -x
# Print versions, esp. versions of dependencies.

python --version
python3 --version

# We use Git during build.
git --version

# This will fail if the build failed.
grass --version
grass --tmp-project XY --exec g.version -ergb
# Detailed Python version info (in one line thanks to echo)
grass --tmp-project XY --exec bash -c "echo Python: \$(\$GRASS_PYTHON -c 'import sys; print(sys.version)')"


cat << EOF > test_r_mapcalc.sh
#!/usr/bin/env bash

g.gisenv set="DEBUG=3"
if ! r.mapcalc "foo = 1"
then
    echo "Failed"
    exit 1
fi
echo "Success"
EOF
chmod +x test_r_mapcalc.sh
grass --tmp-project XY --exec test_r_mapcalc.sh
