CXX := g++-10
CXXFLAGS := -g -O3 -Wall -Wextra # -D__DEBUG_PRT__
TIME := /usr/bin/time -f "%Uu %Ss %Er %Mkb"

MAKEFILE_NAME = ${firstword ${MAKEFILE_LIST}}	# makefile name
OBJECTS = libllheap.o libllheap-stats.o libllheap.so libllheap-stats.so
DEPENDS = ${OBJECTS:.o=.d}			# substitute ".o" with ".d"

.PHONY : all clean				# not file names
.ONESHELL :
.SILENT : test

all : ${OBJECTS}

${OBJECTS} : ${MAKEFILE_NAME}			# OPTIONAL : changes to this file => recompile

libllheap.o : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} -c -o $@ $< -DNDEBUG

libllheap-stats.o : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} -c -o $@ $< -D__DEBUG__ -D__STATISTICS__

libllheap.so : llheap.cc llheap.h
	${CXX} ${CXXFLAGS} -fPIC -shared -o $@ $< -DNDEBUG -DTLS

libllheap-stats.so : llheap.cc llheap.h
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
			echo ${CXX} ${CXXFLAGS} -D`hostname` test.cc libllheap$${sd}.$${lk}o -lpthread $${lk:+-U malloc -Wl,-rpath=.}
			${CXX} ${CXXFLAGS} -D`hostname` test.cc libllheap$${sd}.$${lk}o -lpthread $${lk:+-U malloc -Wl,-rpath=.}
			${TIME} ./a.out
			echo "\n#######################################\n"
		done
	done
