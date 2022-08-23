# babyShell
For this program, you can run a shell that performs basic linux shell tasks, including custom cd, set, export, exit commands.

## To use this shell:
```
$ git clone https://github.com/zw214/babyShell.git
$ cd babyShell
$ ./myShell
```
Now you will see the baby shell is running in your shell, and you can type its supported commands. Basically it should support most of the commands because it will call the function ```execve``` to run uncustomized command, but you can play with the customized command like "cd", "set", "export", and "exit" to test its functionality.

## Shell Functions
1. Initialize with environment variables and set these variables in a map with keys and values;
2. reset all the commands variables and user inputs;
3. read user it, evaluate input variable to see if it is readable;
4. parse user input, create pipes from parent process, iterate on each command;
5. check if the command[0] belongs to customized command. If not, 
6. search each command[0] to see if the path already exists; if it exists, run it by creating fork, redirect command to stdin, stdout, stderr, and execve this command;
7. if the command belongs to customized command, run customized functions.
