# minijzc

sloppy! test compiler
uses c++20, my https://github.com/Mjz86/String (`mjz_lib/` inside it) to
build an optimizing compiler for a simple lang
 
## Build & test

```sh
git clone https://github.com/Mjz86/String mjz_lib   # next to this repo
g++ -std=c++20 -O2 -I. -I mjz_lib main.cpp -o minijz

./minijz --selftest                    # demos + fuzz(200) + native gate
./minijz --fuzz=300 --fuzz-native=8    # wider sweep
```

(`-I.` resolves `mjz_lib/...` includes through the clone above; `-I mjz_lib`
covers the rest.)

The compile-time proof (optional; each check its own g++ invocation
because g++ retains each constant expression's state until end-of-TU):

```sh
for k in 1 2 3 4; do
  g++ -std=c++20 -O0 -I. -I mjz_lib -DMJZ_CX_CHECK=$k \
      -fconstexpr-depth=8192 -fconstexpr-ops-limit=4000000000 \
      -c constexpr_check.cpp -o /dev/null && echo "cx check $k: OK"
done
```


the new task:
make general compile  effiently and actually GENERALIZE the thing.

Analize the splits ,  refactor and make each header consistent ( not dependent on include order ) ,  make composite functions  and split everything to around  50 well named files  and directories that contain  well refactored functions  ,   and  make sure the constexpr test suit is  well rounded,    and generally follow the c++20 core guidelines  , try to use DOD and  make sure most functions have tests if possible,    don't use globals  so constexpr code can run unless there's a necessity  ( eg ,  running the c compiler to emit native )  , and try to fuzz the functions too, after each significant change make a zip of your working directory  to downloads , and at the end  send the zip of the result,  ALSO  , the 2 erlier branches I showed you , make sure their monolithic version matches the  split one , if not , incorporate the diff to the merged program 
 Also ,   see if you can make something that can both be a header only lib for constexpr  test suit , and a multi cpp file build for faster build time , like , the mjz lib is a bit heavy cuz of uts header only Ness  to build , and if a header that used it could be isolated,  then the  rest can just link to the objecf,  u may use cmake , and add automated cmake fuzz testing ect  , having option to fuzz with different seeds  would make more bugs visible  , and have default show cases bring built to see the difference between different versions  ( a git subdirectory in the build folder ) , and overallly  clean up the code for correctness  , do this , so your future  changes   follow the same framework and cleanliness and are easier to understand than searching a monolithic version

