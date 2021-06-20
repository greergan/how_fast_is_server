TARGET=how_fast_is_server

all: clean exe

exe:
	gcc $(TARGET).c -o $(TARGET) -lcurl -lpthread

clean:
	rm -rf $(TARGET)
