CXX := g++-14
CXXFLAGS := -g -O3 -Wall -Wextra # -D__FASTLOOKUP__ -D__OWNERSHIP__ -D__REMOTESPIN__ -D__NONNULL_0_ALLOC__
ifeq ($(shell uname -p),aarch64)		# ARM processor ?
#        CXXFLAGS += -mno-outline-atomics	# use ARM LL/SC instructions for atomics
        CXXFLAGS += -march=armv8.2-a+lse	# use ARM LSE instructions for atomics
endif

LLHEAPFLAGS := -fno-exceptions -fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-stack-check -fno-unwind-tables -fno-rtti #
TIME := /usr/bin/time -f "%Uu %Ss %Er %Mkb"

MAKEFILE_NAME = ${firstword ${MAKEFILE_LIST}}	# makefile name
OBJECTS = libllheap.o libllheap-stats.o libllheap-debug.o libllheap-stats-debug.o \
	  libllheap.so libllheap-stats.so libllheap-debug.so libllheap-stats-debug.so
DEPENDS = ${OBJECTS:.o=.d}			# substitute ".o" with ".d"

.PHONY : all clean				# not file names
.ONESHELL :
.SILENT : test

all : ${OBJECTS}

${OBJECTS} : ${MAKEFILE_NAME}			# OPTIONAL : changes to this file => recompile

libllheap.o : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -c -o $@ $< -DNDEBUG

libllheap-stats.o : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -c -o $@ $< -DNDEBUG -D__STATISTICS__

libllheap-debug.o : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -c -o $@ $< -D__DEBUG__

libllheap-stats-debug.o : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -c -o $@ $< -D__DEBUG__ -D__STATISTICS__

libllheap.so : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -fPIC -shared -o $@ $< -DNDEBUG -DTLS

libllheap-stats.so : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -fPIC -shared -o $@ $< -DNDEBUG -D__STATISTICS__ -DTLS

libllheap-debug.so : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -fPIC -shared -o $@ $< -D__DEBUG__ -DTLS

libllheap-stats-debug.so : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} ${LLHEAPFLAGS} -fPIC -shared -o $@ $< -D__DEBUG__ -D__STATISTICS__ -DTLS

clean :
	rm -f ${OBJECTS} a.out

# testgen.cc testllheap.cc
testpgm := testgen.cc

test : ${OBJECTS}
#	set -x
	if  [ "${testpgm}" != "testllheap.cc" ] ; then
		echo "\nDefault allocator"
		echo ${CXX} ${CXXFLAGS} -D`hostname` ${testpgm} -lpthread
		${CXX} ${CXXFLAGS} -D`hostname` ${testpgm} -lpthread
		${TIME} ./a.out
		echo "\n#######################################\n"
	fi
	for lk in "" "s" ; do
		for sd in "" "-stats" "-debug" "-stats-debug" ; do
			echo ".$${lk}o linkage, $${sd:-no stats}"
			echo ${CXX} ${CXXFLAGS} -D`hostname` ${testpgm} libllheap$${sd}.$${lk}o -lpthread $${lk:+-U malloc -Wl,-rpath=.}
			${CXX} ${CXXFLAGS} -D`hostname` ${testpgm} libllheap$${sd}.$${lk}o -lpthread $${lk:+-U malloc -Wl,-rpath=.}
			${TIME} ./a.out
			echo "\n#######################################\n"
		done
	done
