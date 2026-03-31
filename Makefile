CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2

SRCS   = ac_utils.cpp ac_csv.cpp ac_encoder.cpp ac_serial.cpp main.cpp
OBJS   = $(SRCS:.cpp=.o)
TARGET = compress

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# Explicit header dependencies so make rebuilds the right objects on header changes.
ac_utils.o:   ac_utils.cpp   ac_utils.h   ac_types.h
ac_csv.o:     ac_csv.cpp     ac_csv.h
ac_encoder.o: ac_encoder.cpp ac_encoder.h ac_types.h ac_utils.h
ac_serial.o:  ac_serial.cpp  ac_serial.h  ac_types.h
main.o:       main.cpp       ac_csv.h     ac_encoder.h ac_serial.h ac_types.h
