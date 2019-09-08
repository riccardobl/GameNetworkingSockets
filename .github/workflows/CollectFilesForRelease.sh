#!/bin/bash
set -e 

if [ "$LIBRARY_EXT" = "" ];
then
  export LIBRARY_EXT="so"
fi

if [ "$DIST_PATH" = "" ];
then
  export DIST_PATH=$PWD/dist 
fi

if [ "$EXAMPLES_PATH" = "" ];
then
  export EXAMPLES_PATH=$PWD/dist_examples
fi

if [ "$TESTS_PATH" = "" ];
then
  export TESTS_PATH=$PWD/dist_tests 
fi

if [ "$OUT_PATH" = "" ];
then
  export OUT_PATH=$PWD/out
fi

if [ "$HEADERS_PATH" = "" ];
then
  export HEADERS_PATH=$PWD/dist_headers
fi

mkdir -p $DIST_PATH
mkdir -p $EXAMPLES_PATH 
mkdir -p $TESTS_PATH 
mkdir -p $HEADERS_PATH

echo "Collect libraries" 
cp -Rvf build/src/*.$LIBRARY_EXT $DIST_PATH/ 
cp -Rvf ./protobuf-amd64/bin/*protobuf.$LIBRARY_EXT  $DIST_PATH/ || true  
cp -Rvf ./protobuf-amd64/lib/*protobuf.$LIBRARY_EXT  $DIST_PATH/ || true 
ls $DIST_PATH/

echo "Collect examples" 
cp -Rvf $DIST_PATH/* $EXAMPLES_PATH/ 
rm -Rf build/examples/CMakeFiles 
rm -Rf build/examples/CMakeLists.txt 
rm -Rf build/examples/*.cmake 
cp -Rf build/examples/* $EXAMPLES_PATH/ 
ls $EXAMPLES_PATH/ 

echo "Collect tests" 
cp -Rvf $DIST_PATH/* $TESTS_PATH/     
rm -Rf build/tests/CMakeFiles 
rm -Rf build/tests/CMakeLists.txt 
rm -Rf build/tests/*.cmake 
cp -Rvf build/tests/* $TESTS_PATH/     
ls $TESTS_PATH/    

echo "Collect headers"
cp -Rvf include/* $HEADERS_PATH/
ls  $HEADERS_PATH/

7z a -r "$OUT_PATH/examples.zip" $EXAMPLES_PATH/*
7z a -r "$OUT_PATH/lib.zip" $DIST_PATH/*
7z a -r "$OUT_PATH/tests.zip" $TESTS_PATH/*
7z a -r "$OUT_PATH/headers.zip" $HEADERS_PATH/*

