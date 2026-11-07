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


