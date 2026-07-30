CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -Isrc

BUILD    = build

# Library modules (src/). CORE is everything the compress/decompress path needs;
# QUERY adds the operators only bench exercises.
CORE_OBJS  = $(BUILD)/ac_utils.o $(BUILD)/ac_csv.o \
             $(BUILD)/ac_encoder.o $(BUILD)/ac_bloom.o \
             $(BUILD)/ac_decompressor.o $(BUILD)/ac_serial.o

QUERY_OBJS = $(BUILD)/ac_random_access.o $(BUILD)/ac_scanner.o

BINARIES   = $(BUILD)/compress $(BUILD)/decompress $(BUILD)/bench

.PHONY: all clean
all: $(BINARIES)

$(BUILD)/compress:   $(CORE_OBJS) $(BUILD)/main.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/decompress: $(CORE_OBJS) $(BUILD)/main_decompress.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/bench:      $(CORE_OBJS) $(QUERY_OBJS) $(BUILD)/bench.o
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: tools/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

# Explicit header dependencies so make rebuilds the right objects on header changes.
$(BUILD)/ac_utils.o:         src/ac_utils.cpp         src/ac_utils.h        src/ac_types.h
$(BUILD)/ac_csv.o:           src/ac_csv.cpp           src/ac_csv.h
$(BUILD)/ac_encoder.o:       src/ac_encoder.cpp       src/ac_encoder.h      src/ac_types.h src/ac_utils.h src/ac_bloom.h
$(BUILD)/ac_bloom.o:         src/ac_bloom.cpp         src/ac_bloom.h
$(BUILD)/ac_decompressor.o:  src/ac_decompressor.cpp  src/ac_decompressor.h src/ac_types.h
$(BUILD)/ac_serial.o:        src/ac_serial.cpp        src/ac_serial.h       src/ac_types.h
$(BUILD)/ac_random_access.o: src/ac_random_access.cpp src/ac_random_access.h src/ac_types.h
$(BUILD)/ac_scanner.o:       src/ac_scanner.cpp       src/ac_scanner.h      src/ac_types.h src/ac_random_access.h
$(BUILD)/main.o:             tools/main.cpp             src/ac_csv.h src/ac_encoder.h src/ac_serial.h src/ac_types.h
$(BUILD)/main_decompress.o:  tools/main_decompress.cpp  src/ac_decompressor.h src/ac_serial.h src/ac_types.h
$(BUILD)/bench.o:            tools/bench.cpp            src/ac_csv.h src/ac_encoder.h src/ac_decompressor.h src/ac_random_access.h src/ac_scanner.h src/ac_serial.h src/ac_types.h
