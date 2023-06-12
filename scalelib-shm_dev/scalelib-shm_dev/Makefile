OBJ = client.o scalelib.o get_clock.o shared_memory.o
CXXFLAGS = -std=gnu++11 -O3 -g

client: $(OBJ)
	g++ -g -o client $(OBJ) -libverbs -lpthread -lrt 
	# g++ -o client $(OBJ) -lrdmacm -libverbs -lpthread -lrt 

client.o: client.cpp scalelib.h common.h log.h types.h get_clock.h shared_memory.h arch.h
scalelib.o: scalelib.cpp
shared_memory.o: shared_memory.cpp

# rdma_api.o: rdma_api.cpp
# shared_memory.o: shared_memory.cpp
# verbs_cmd.o: verbs_cmd.cpp


clean:
	rm client *.o