#include "myShell.h"
#include <cstdlib>
#include <string>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <iterator>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <fcntl.h>

extern char ** environ;

/**************************/
/******STATIC VARIABLE*****/
/**************************/

std::map<std::string, MyShell::Command_Function_Pointer> MyShell::COMMAND_MAP = {
    {"exit", &MyShell::runExitCommands},
    {"cd", &MyShell::runCdCommand},
    {"set", &MyShell::runSetCommand},
    {"export", &MyShell::runExportCommand},
};

/***************************/
/******HELPER FUNCTIONS*****/
/***************************/

/**
 * a valid variable name can only contain upper and lower letters, numbers, and underscores
 * 
 */
bool validateVarName(std::string var) {
    for (std::string::iterator it = var.begin(); it != var.end(); ++it) {
        if (!isalnum(*it) && *it != '_') return false;
    }
    return true;
}

/**
 * transform the parsed command from a vector of strings into an array of char pointers,
 * which is required by the execve function
 * the 2d array is dynamically allocated, needs to be freed manually after use
 */
char ** vector2array(std::vector<std::string> v) {
    char ** array = new char*[v.size() + 1];
    std::size_t count = 0;
    for (std::vector<std::string>::iterator it = v.begin(); it != v.end(); ++it) {
        array[count] = new char[it->length() + 1];
        strncpy(array[count], it->c_str(), it->length() + 1);
        count++;
    }
    array[count] = NULL;
    return array;
}

/**
 * split the colon delimited PATH string into a vector of strings on colon
 * 
 */
std::vector<std::string> splitPath(const std::string & paths) {
    std::vector<std::string> paths_vector;
    std::istringstream iss(paths); // create a istringstream object
    std::string path;
    while (std::getline(iss, path, ':')) paths_vector.push_back(path);
    return paths_vector;
}

/**********************************/
/******CLASS PRIVATE FUNCTIONS*****/
/**********************************/ 

/**
 * set the key-value pair into MyShell::vars
 * this function does not check if the variable name is valid
 * it is the caller's job to guarantee the variable name is valid
 * 
 */
void MyShell::setVar(std::string key, std::string value) {
    vars[key] = value;
}

/**
 * evaluate any variable names MyShell::input
 * variable names are started with '$' and lasts as long as it is still a valid var name
 * if the variable exists, replace it with its value
 * if the variable does not exist, replace the variable name with ""
 */
void MyShell::evaluateVars() {
    std::string new_input;
    for (std::size_t i = 0; i < input.size(); ) {
        if (input[i] != '$') { // characters after $
            new_input.push_back(input[i]);
            i++;
        }
        else {
            std::size_t start = i + 1;
            std::size_t len = 0;
            // incrementing len
            while (start + len < input.size() && validateVarName(input.substr(start, len + 1))) len++;
            std::string var_name = input.substr(start, len);
            std::string var_value = "";
            std::map<std::string, std::string>::iterator it = vars.find(var_name);
            if (it != vars.end()) var_value = it->second;
            new_input.append(var_value);
            i = start + len;
        }
    }
    input = new_input;
}

/**
 * parse the one-line user input on '|' to get each piped command
 * store all piped commands into std::vector<std::string> MyShell::piped_commands
 * if the last character in MyShell::input is '|', this is regarded as error
 */

void MyShell::parsePipedInput() {
    std::size_t start = 0, pipe_pos = 0;
    // find at or after 'start'
    while ((pipe_pos = input.find('|', start)) != std::string::npos) {
        piped_commands.push_back(input.substr(start, pipe_pos - start));
        start = pipe_pos + 1;
    }
    if (pipe_pos == input.length() - 1) {
        std::cerr << "cannot have | at the end of input" << std::endl;
        error = true;
        return;
    }
    // push_back the last user input after '|'
    piped_commands.push_back(input.substr(start));
}

/**
 * parse piped_commands[curr_command_index]
 * command is delimitered by whitespace, unless the whitespace is escaped, whitespace will be discarded
 * the first delimited "word" in the command is the command name
 * all others "words" are arguments to the word
 * the \ mark will be removed from the user input command string for the ease of future use
 * the MyShell::commands vector could be empty if the input is empty
 * 
 */
void MyShell::parseCommand() {
    std::string curr_command = piped_commands[curr_command_index];
    std::string word, modified_curr_command;
    for (std::size_t end = 0; end < curr_command.size(); ++end) {
        if (curr_command[end] == ' ') { // if it is whitespace
            if ((end > 0 && curr_command[end-1] != ' ') || (end > 1 && curr_command[end-2] == '\\')) {
                commands.push_back(std::string(word));
                word.erase(); // word here acts like a buffer
            }
            modified_curr_command.push_back(curr_command[end]);
        }
        else if (curr_command[end] == '\\') {
            if (end == curr_command.size() - 1) {
                std::cerr << "cannot use escape mark at the end of a command" << std::endl;
                error = true;
                return;
            }
            word.push_back(curr_command[++end]); // put the escaped char into the word stream
            modified_curr_command.push_back(curr_command[end]); // remove the escape mark from the input command
            if (end == curr_command.size() - 1) commands.push_back(std::string(word));
            // normal non-space and non-escape chars
        }
        else {
            word.push_back(curr_command[end]);
            modified_curr_command.push_back(curr_command[end]);
            if (end == curr_command.size() - 1) commands.push_back(std::string(word));
        }
    }
    piped_commands[curr_command_index] = modified_curr_command;
}

/**
 * search if the given path for the command really exists
 * commands guaranteed to be non-empty
 * commands[0] is the real command that we care about
 * 1. commands[0] contain '/': find if the file name exist
 * 2. commands[0] does not contain '/': search all paths in PATH, to see if the file name is in any one of them
 * in the two cases, if the file is found, return true, else return false
 * 
 */
bool MyShell::searchCommand() {
    std::string command = commands[0];
    std::vector<std::string> paths = splitPath(vars["PATH"]);
    bool exist = false; // initialize to false
    if (command.find('/') != std::string::npos) {
        std::ifstream ifs(command.c_str()); // ifstream: input stream class to operate on files
        if (ifs.good()) exist = true;
    }
    else {
        for (std::vector<std::string>::iterator it = paths.begin(); it != paths.end(); ++it) {
            std::string complete_path = *it + "/" + command;
            std::ifstream ifs(complete_path.c_str());
            if (ifs.good()) {
                exist = true;
                commands[0] = complete_path; // replace the shortened path with the complete one
                break;
            }
        }
    }
    return exist;
}


/**
 * run exit commands (EOF and "exit")
 * set the boolean variable exitting to true
 */
void MyShell::runExitCommands() {
    exitting = true;
}

/**
 * run "cd" command
 * cd command can only take 0 or 1 argument
 * 0 argument: cd to home directory
 * 1 argument: cd to the given argument if it exists
 */
void MyShell::runCdCommand() {
    if (commands.size() > 2) {
        std::cerr << "too many arguments for cd" << std::endl;
        error = true;
        return;
    }
    std::string dest = getenv("HOME");
    if (commands.size() == 2) dest = commands[1];
    // chdir fails, the current directory doesn't change
    if (chdir(dest.c_str()) != 0) {
        std::cerr << "cannot change directory: " << std::strerror(errno) << std::endl;
        error = true;
    }
    else {
        char cwd[1024];
        setVar("OLDPWD", vars["PWD"]);
        setenv("OLDPWD", vars["PWD"].c_str(), 1);
        setVar("PWD", getcwd(cwd, sizeof(cwd))); // get absolute working path in the case
        setenv("PWD", vars["PWD"].c_str(), 1);
    }
}

/**
 * run "set" command
 * the syntax has to be: set "variableName" "variableValue"
 * and there are a set of rules for valid variable name, refer to document
 * 
 */
void MyShell::runSetCommand() {
    if (commands.size() < 3) {
        std::cerr << "too few arguments for set: " << commands.size() << std::endl;
        error = true;
        return;
    }
    if (!validateVarName(commands[1])) {
        std::cerr << "invalid var name: var names can only contain letters (case sensitive), numbers and underscores" << std::endl;
        error = true;
        return;
    }
    std::size_t setPos = input.find(commands[0]); // the starting index of substr "set"
    std::size_t varPos = input.find(commands[1], setPos + commands[0].size()); // the starting index of var name, with a valid var name, find won't have strange behavior
    std::size_t valPos = varPos + commands[1].size() + 1; // in this case, the starting index of value is 1 space after var name
    std::string value = input.substr(valPos);
    setVar(commands[1], value);
    std::cout << "set variable " << commands[1] << " with value " << vars[commands[1]] << std::endl;
}

/**
 * run "export" command
 * the "export" command can take any number of variables, and export each of them into environ
 * but if any of the variable names is invalid, the export process stops right at it, vars before it are exported, but all later vars (including itself) are not exported
 * if the exported var is in MyShell::vars, just export it into environ, do nothing else
 * else add the var and "" as its value into MyShell::vars, and then export it into environ
 */
void MyShell::runExportCommand() {
    for (std::vector<std::string>::iterator it = commands.begin() + 1; it != commands.end(); ++it) {
        if (!validateVarName(*it)) {
            std::cerr << "invalid var name: var names can only contain letters (case sensitive), numbers, and underscores" << std::endl;
            error = true;
            return;
        }
        std::map<std::string, std::string>::iterator varsit = vars.find(*it);
        std::string value = "";
        if (varsit != vars.end()) value = varsit->second;
        else setVar(*it, "");
        if (setenv(it->c_str(), value.c_str(), 1) != 0) {
            std::cerr << "failed to export variable " << *it << ": " << std::strerror(errno) << std::endl;
            error = true;
        }
        else std::cout << "export variable " << *it << " with value " << value << std::endl;
    }
}

/**
 * in a child process
 * config redirect input and output
 * <: redirect stdin to the given input file, if the file does not exist, create with permissions
 * >: redirect stdout to the given output file, if the file does not exist, create with permissions
 * 2>: rediect stderr to the given output file, if the file does not exist, create with permissions
 * 2>&1: redirect stderr to the same file as the output file
 * TODO refactor, extract methods
 * 
 */
void MyShell::configCommandRedirect() {
    std::string input_filename;
    std::string output_filename;
    std::string error_filename;
    for (std::vector<std::string>::iterator it = commands.begin() + 1; it != commands.end(); ) {
        std::string curr = *it;
        if (curr.compare(0, 1, "<") == 0) {
            if (curr.size() == 1) { // curr == "<"
                if (it + 1 == commands.end()) {
                    std::cerr << "incorrect input format: < requires an input file" << std::endl;
                    exit(EXIT_FAILURE);
                }
                else {
                    input_filename = *(it + 1);
                    commands.erase(it);
                    commands.erase(it); // no need for it++ again
                }
            }
            else { // curr starts with "<"
                input_filename = curr.substr(1);
                commands.erase(it); // no need for it++ again
            }
        }
        else if (curr.compare(0, 1, ">") == 0) {
            if (curr.size() == 1) {
                if (it + 1 == commands.end()) {
                    std::cerr << "incorrect input format: > requires an output file" << std::endl;
                    exit(EXIT_FAILURE);
                }
                else {
                    output_filename = *(it + 1);
                    commands.erase(it);
                    commands.erase(it); // no need for it++ again
                }
            }
            else {
                output_filename = curr.substr(1);
                commands.erase(it); // no need for it++ again
            }
        }
        else if (curr.compare(0, 2, "2>") == 0) {
            if (curr.size() == 2){
                if (it + 1 == commands.end()) {
                    std::cerr << "incorrect input format: 2> requires an output file" << std::endl;
                    exit(EXIT_FAILURE);
                }
                else {
                    error_filename = *(it + 1);
                    commands.erase(it);
                    commands.erase(it); // no need for it++ again
                }
            }
            else {
                if (curr.compare("2>&1") == 0) error_filename = output_filename;
                else error_filename = curr.substr(2);
            }
            commands.erase(it); // no need for it++ again
        }
        else it++;
    }
    if (!input_filename.empty()) {
        if (curr_command_index != 0) { // only the first piped command can redirect stdin
            std::cerr << "cannot redirect stdin for a non-head command in pipe" << std::endl;
            exit(EXIT_FAILURE);
        }
        close(0);
        if (open(input_filename.c_str(), O_RDONLY, 0) < 0) {
            std::cerr << "cannot open the redirect input file: " << std::strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    if (!output_filename.empty()) {
        if (curr_command_index != piped_commands.size() - 1) { // only the last piped command can redirect stdout
            std::cerr << "cannot redirect stdout for a non-end command in pipe" << std::endl;
            exit(EXIT_FAILURE);
        }
        close(1);
        // set file permission
        if (open(output_filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) < 0) {
            std::cerr << "cannot open the redirect output file: " << std::strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    if (!error_filename.empty()) {
        close(2);
        if (open(error_filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666) < 0) {
            std::cerr << "cannot open the redirect error file: " << std::strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * in a child process
 * config its pipe input and output
 */
void MyShell::configCommandPipe(bool redirect_input = true, bool redirect_output = true) {
    std::size_t num_commands = piped_commands.size();
    std::size_t num_pipes = num_commands - 1;
    if (redirect_input) {
        if (curr_command_index != 0) { // not the first command
        // close fd 0 for stdin, and redirect pipe R end to fd 0
            if (dup2(pipefd[2 * (curr_command_index - 1)], 0) < 0) {
                std::cerr << "failed to redirect stdin: " << std::strerror(errno) << std::endl;
                exit(EXIT_FAILURE);
            }
        }
    }
    if (redirect_output) {
        if (curr_command_index != num_commands - 1) { // not the last command
        // close fd 1 for stdout, and redirect pipe W end to fd 1
            if (dup2(pipefd[2 * curr_command_index + 1], 1) < 0) {
                std::cerr << "failed to redirect stdout: " << std::strerror(errno) << std::endl;
                exit(EXIT_FAILURE);
            }
        }
    }
    // close all pipe fds, only use the new 0 and 1 for read and write
    for (std::size_t i = 0; i < 2 * num_pipes; ++i) {
        if (close(pipefd[i]) < 0) {
            std::cerr << "failed to close pipes: " << std::strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * run a normal command with the need to fork a child process
 * (commands except for exit, cd, set and export)
 */
void MyShell::runCommand() {
    pid_t forkResult = fork(); // fork a child process same as the parent one
    num_child_processes++;
    if (forkResult == -1) { // fork error, skip the command 
        std::cerr << "failed to create a child process: " << std::strerror(errno) << std::endl;
        error = true;
    }
    else if (forkResult == 0) {
        configCommandRedirect();
        configCommandPipe();
        char ** c_commands = vector2array(commands);
        // if the child calls execve right after forking, the second copy of the parent's memory is destroyed and replaced
        // with a memory image loaded from the requested binary
        execve(c_commands[0], c_commands, environ); // execve returns only if there is an error, so it is called once and never returns
        std::cerr << "execve failed: " << std::strerror(errno) << std::endl;
        _exit(EXIT_FAILURE); // if execve returns, the child process fails, and should use _exit to exit the forked child process
    }
}

/**
 * in the parent process
 * allocate space for MyShell::pipefd
 * create pipes for all piped commands
 */
void MyShell::createPipes() {
    int num_pipes = piped_commands.size() - 1;
    pipefd = new int[2 * num_pipes]; // 2 * number of pipe marks, R and W
    for (std::size_t i = 0; i < num_pipes; ++i) {
        if (pipe(pipefd + 2 * i) < 0) { // R end: 2 * i, W end: 2 * i + 1
            std::cerr << "failed to create pipes: " << std::strerror(errno) << std::endl;
            error = true;
            return;
        }
    }
}

/**
 * in the parent process
 * close all pipe fds before waiting for child processes
 */
void MyShell::closePipes() {
    int num_pipes = piped_commands.size() - 1;
    for (std::size_t i = 0; i < 2 * num_pipes; i++) {
        if (close(pipefd[i]) < 0) {
            std::cerr << "failed to close pipe " << i << ": " << std::strerror(errno) << std::endl;
            error = true;
            return;
        }
    }
}

/**
 * in the parent process
 * the parent needs to wait for all its forked child processes to finish (exit), and then exit itself
 * report the exit status of the last piped command, if that command has a exit status (needs fork)
 */
void MyShell::waitForChildProcesses() {
    int childStatus;
    for (std::size_t i = 0; i < num_child_processes; ++i) {
        wait(&childStatus);
        if (i == num_child_processes - 1) { // only print the exit status of the last "reportable" piped command
            // the child process is terminated normally
            if (WIFEXITED(childStatus)) std::cout << "Program exited with status: " << WEXITSTATUS(childStatus) << std::endl;
            // the child process is terminated due to receipt of a signal
            else if (WIFSIGNALED(childStatus)) std::cout << "Program was killed by signal " << WTERMSIG(childStatus) << std::endl;
        }
    }
}

/**
 * run the piped commands in MyShell::piped_commands in order
 */
void MyShell::runPipedCommands() {
    // create pipes in the parent process
    createPipes();
    for (curr_command_index = 0; curr_command_index < piped_commands.size(); ++curr_command_index) {
        if (error) break; // if any error occur previously during the execution of this input, stop
        parseCommand();
        if (error) break; // if error occur during parsing command, stop
        if (!commands.empty()) { // if command is not empty
            std::string command_name = commands[0];
            std::map<std::string, Command_Function_Pointer>::iterator it = COMMAND_MAP.find(command_name);
            if (it == COMMAND_MAP.end()) { // normal command
                if (searchCommand()) { // command exists
                    runCommand();
                }
                else {
                    std::cerr << "command " << commands[0] << " not found" << std::endl;
                    error = true;
                }
            }
            else { // one of the 4 special commands
                Command_Function_Pointer cfp = it->second;
                (this->*cfp)();
            }
            commands.clear();
        }
    }
    // order is important here, parent should first close pipes, and then wait for child processes
    closePipes();
    waitForChildProcesses();
    delete[] pipefd; // free memory
}

/**
 * set the error indicator to false
 * remove contents of input, piped_commands and commands
 * reset curr_command_index and num_child_processes to 0
 * update environment variables in case other programs change them
 */
void MyShell::refresh() {
    error = false;
    input.clear();
    piped_commands.clear();
    commands.clear();
    curr_command_index = 0;
    num_child_processes = 0;
}

/**********************************/
/*******CLASS PUBLIC FUNCTIONS*****/
/**********************************/ 

/**
 * default constructor of MyShell CLASS
 * initialize some class variables
 * set up env vars once
 */
MyShell::MyShell(): error(false), exitting(false), curr_command_index(0), num_child_processes(0) {
    // environment is represented as an array of strings. Each string is of the format 'name=value'. 
    // The last element of the array is a null pointer
    // Variable is declared in header file unistd.h.
    char ** envp = environ;
    while (*envp) {
        std::string curr_env(*envp);
        envp++;
        std::size_t equal_index = curr_env.find('=');
        setVar(curr_env.substr(0, equal_index), curr_env.substr(equal_index + 1));
    }
}

/**
 * execute one round of input command
 */
void MyShell::execute() {
    // reset
    refresh();
    // get PWD
    std::cout << "myShell:" << vars["PWD"] << "$ ";
    std::getline(std::cin, input); //default delim is '\n' and will be discarded, great!
    if (std::cin.eof()) {
        runExitCommands();
        std::cin.clear();
        std::cout << std::endl;
    }
    else if (std::cin.fail() || std::cin.bad()) std::cin.clear();
    else { // std::cin is good, input is valid
        evaluateVars(); // first of all, evaluate the vars in input
        parsePipedInput();
        if (error) return; // if there's any error, return already
        runPipedCommands();
    }
}

/**
 * return to the caller if the shell is going to exit
 * i.e., in the previous step the user gives "exit" commands
 */
bool MyShell::isExitting() {
    return exitting;
}