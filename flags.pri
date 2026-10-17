# Compiler flags shared by the app and its tests, so what's tested is built
# as what's shipped.
# The simulations and picture work are tight loops over grids and pixels,
# which GCC vectorizes fully only at -O3 (no fast-math, so results are the
# same to the bit).
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -O3
