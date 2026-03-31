CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2

# Shared object files (used by both binaries).
SHARED_OBJS = ac_utils.o ac_csv.o ac_encoder.o ac_decompressor.o ac_serial.o

all: compress decompress

compress: $(SHARED_OBJS) main.o
	$(CXX) $(CXXFLAGS) -o $@ $^

decompress: $(SHARED_OBJS) main_decompress.o
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(SHARED_OBJS) main.o main_decompress.o compress decompress

# Explicit header dependencies so make rebuilds the right objects on header changes.
ac_utils.o:        ac_utils.cpp        ac_utils.h        ac_types.h
ac_csv.o:          ac_csv.cpp          ac_csv.h
ac_encoder.o:      ac_encoder.cpp      ac_encoder.h      ac_types.h ac_utils.h
ac_decompressor.o: ac_decompressor.cpp ac_decompressor.h ac_types.h
ac_serial.o:       ac_serial.cpp       ac_serial.h       ac_types.h
main.o:            main.cpp            ac_csv.h          ac_encoder.h ac_serial.h ac_types.h
main_decompress.o: main_decompress.cpp ac_decompressor.h ac_serial.h  ac_types.h
