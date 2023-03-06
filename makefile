target=app
CppSrc=$(wildcard *.cpp ./nonactive/*.cpp ./request/*.cpp ./threadpool/*.cpp)
HeaderSrc=$(wildcard *.h ./nonactive/*.h ./request/*.h ./threadpool/*.h)

target: $(CppSrc) $(HeaderSrc)
	$(CXX)  -o $(target) $(CppSrc) $(HeaderSrc) -g -lpthread