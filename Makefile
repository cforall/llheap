CXX := g++-10
CXXFLAGS := -g -O3 -Wall -Wextra # -D__DEBUG_PRT__
TIME := /usr/bin/time -f "%Uu %Ss %Er %Mkb"

MAKEFILE_NAME = ${firstword ${MAKEFILE_LIST}}	# makefile name
OBJECTS = libhThread.o libhThread-stats.o libhThread.so libhThread-stats.so
DEPENDS = ${OBJECTS:.o=.d}			# substitute ".o" with ".d"

.PHONY : all clean				# not file names
.ONESHELL :
.SILENT : test

all : ${OBJECTS}

${OBJECTS} : ${MAKEFILE_NAME}			# OPTIONAL : changes to this file => recompile

libhThread.o : HeapPerThread.cc HeapPerThread.h
	${CXX} ${CXXFLAGS} -c -o $@ $< -DNDEBUG

libhThread-stats.o : HeapPerThread.cc HeapPerThread.h
	${CXX} ${CXXFLAGS} -c -o $@ $< -D__DEBUG__ -D__STATISTICS__

libhThread.so : HeapPerThread.cc HeapPerThread.h
	${CXX} ${CXXFLAGS} -fPIC -shared -o $@ $< -DNDEBUG -DTLS

libhThread-stats.so : HeapPerThread.cc HeapPerThread.h
	${CXX} ${CXXFLAGS} -fPIC -shared -o $@ $< -D__DEBUG__ -D__STATISTICS__ -DTLS

clean :
	rm -f ${OBJECTS} a.out

test : ${OBJECTS}
#	set -x
	echo "\nDefault allocator"
	echo ${CXX} ${CXXFLAGS} -D`hostname` test.cc -lpthread
	${CXX} ${CXXFLAGS} -D`hostname` test.cc -lpthread
	${TIME} ./a.out
	echo "\n#######################################\n"
	for lk in "" "s" ; do
		for sd in "" "-stats" ; do
			echo ".$${lk}o linkage, $${sd:-no stats}"
			echo ${CXX} ${CXXFLAGS} -D`hostname` test.cc libhThread$${sd}.$${lk}o -lpthread $${lk:+-U malloc -Wl,-rpath=$${HOME}/heap -L${HOME}/heap}
			${CXX} ${CXXFLAGS} -D`hostname` test.cc libhThread$${sd}.$${lk}o -lpthread $${lk:+-U malloc -Wl,-rpath=$${HOME}/heap -L${HOME}/heap}
			${TIME} ./a.out
			echo "\n#######################################\n"
		done
	done
