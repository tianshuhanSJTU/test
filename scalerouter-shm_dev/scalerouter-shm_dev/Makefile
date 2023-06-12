OBJ = server.o scalerouter.o shared_memory.o
CXXFLAGS = -std=gnu++11 -O3
client: $(OBJ)
	g++ -o server $(OBJ) -libverbs -lpthread -lrt
	# g++ -o server $(OBJ) -lrdmacm -libverbs -lpthread -lrt 
server.o: server.cpp scalerouter.h common.h log.h types.h shared_memory.h arch.h
scalerouter.o: scalerouter.cpp
shared_memory.o: shared_memory.cpp
# rdma_api.o: rdma_api.cpp
# verbs_cmd.o: verbs_cmd.cpp

clean:
	rm server *.o

