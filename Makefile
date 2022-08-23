myShell: src/main.cpp src/myShell.cpp src/myShell.h
		g++ -std=gnu++11 -Wall -Werror -pedantic -o myShell src/main.cpp src/myShell.cpp
clean:
		rm myShell *~