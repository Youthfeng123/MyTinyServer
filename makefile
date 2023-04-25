target=app
CppSrc=$(wildcard *.cpp ./nonactive/*.cpp ./request/*.cpp ./threadpool/*.cpp ./mysqlpool/*.cpp)
HeaderSrc=$(wildcard *.h ./nonactive/*.h ./request/*.h ./threadpool/*.h ./mysqlpool/*.h)

target: $(CppSrc) $(HeaderSrc)
	$(CXX)  -o $(target) $(CppSrc) $(HeaderSrc) -g -lpthread -lmysqlclient
