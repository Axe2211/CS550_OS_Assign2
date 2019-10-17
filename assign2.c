#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>


//limits
#define MAX_TOKENS 100
#define MAX_STRING_LEN 100
#define MAX_BG_TASKS 100
size_t MAX_LINE_LEN = 10000;


// commands
#define EXIT_STR "exit"
#define EXIT_CMD 1
#define FG_STR "fg"
#define FG_CMD 2
#define LISTJOBS_STR "listjobs"
#define LISTJOBS_CMD 3
#define IO (check_inSymbol() > 0) && (check_outSymbol() > 0)
#define INPUT (check_inSymbol() > 0) && (check_outSymbol() < 0)
#define OUTPUT (check_inSymbol() < 0) && (check_outSymbol() > 0)

// process states
#define RUNNING 0
#define TERMINATED 1
#define STOPPED 2
static const char *status_string[] = {"RUNNING", "TERMINATED", "STOPPED"};

// misc
#define ERROR -1
#define FREE -1



FILE *fp; // file struct for stdin
char **tokens;
int token_count = 0;
char ***commands;
int command_index;
char *line;
char *input_file;
char *output_file;

struct background_task {
	pid_t pid;
	int status;
} bgtask[MAX_BG_TASKS];
int bg_count = 0;

void initialize()
{

	// allocate space for the whole line
	assert( (line = malloc(sizeof(char) * MAX_STRING_LEN)) != NULL);

	// allocate space for individual token pointers
	assert( (tokens = malloc(sizeof(char *) * MAX_TOKENS)) != NULL);

	// open stdin as a file pointer 
	assert( (fp = fdopen(STDIN_FILENO, "r")) != NULL);

	// cleanup background task structs
	bg_count = 0;
	for(int i=0; i<MAX_BG_TASKS; i++) 
		bgtask[i].pid = FREE; 
}

void tokenize (char * string)
{
	int size = MAX_TOKENS;
	char *this_token;

	// cleanup old token pointers
	for(int i=0; i<token_count; i++) tokens[i] = NULL;

	token_count = 0;
	while ( (this_token = strsep( &string, " \t\v\f\n\r")) != NULL) {

		if (*this_token == '\0') continue;

		tokens[token_count] = this_token;

		//printf("Token %d: %s\n", token_count, tokens[token_count]);

		token_count++;

		// if there are more tokens than space ,reallocate more space
		if(token_count >= size){
			size*=2;

			assert ( (tokens = realloc(tokens, sizeof(char*) * size)) != NULL);
		}
	}
	//printf("token count = %d\n", token_count);
}

void read_command() 
{

	// getline will reallocate if input exceeds max length
	assert( getline(&line, &MAX_LINE_LEN, fp) > -1); 

	//printf("Shell read this line: %s\n", line);

	tokenize(line);
}


void handle_exit() 
{
	//printf("Exit command\n");
	
	// cleanup terminated tasks
	for(int i=0; i<MAX_BG_TASKS; i++) {
		if( bgtask[i].pid == FREE) continue;

		if (waitpid( bgtask[i].pid, NULL, WNOHANG) < 0)
			perror("error cleaning up background task:");
	}
}

void handle_listjobs() 
{
	int ret, status;

	for(int i=0; i<MAX_BG_TASKS; i++) {
		if( bgtask[i].pid == FREE) continue;

		//update status
		ret = waitpid( bgtask[i].pid, &status, WNOHANG);

		if (ret > 0) {

			if(WIFEXITED(status) || WIFSIGNALED(status)) 
				bgtask[i].status = TERMINATED;

			if(WIFSTOPPED(status)) 
				bgtask[i].status = STOPPED;
		}

		printf("PID: %d Status: %s\n", bgtask[i].pid, status_string[bgtask[i].status] );

		if(bgtask[i].status == TERMINATED) {
			bgtask[i].pid = FREE;
			bg_count--;
		}
	}
	
}

void handle_fg() 
{
	int i;
	pid_t  pid;
	int status;

	if(tokens[1] == NULL) {
		printf("Usage: fg <pid>\n");
		return;
	}

	pid = atoi(tokens[1]);

	for(i=0; i<MAX_BG_TASKS; i++) {
		if( bgtask[i].pid == pid ) break;
	}

	if(i == MAX_BG_TASKS) {
		printf("PID %d: No such background task\n", pid);
		return;
	}

	// free the entry
	bgtask[i].pid = FREE;

	//handle_listjobs();

	// block
	if( waitpid( pid, &status, 0) < 0) {
		perror("error waiting on forgrounded process:");
	}

}


int next_free_bg() 
{
	for(int i=0; i<MAX_BG_TASKS; i++) 
		if(bgtask[i].pid == FREE) return i;

	return -1;
}

int is_background() 
{
	return (strcmp(tokens[token_count-1], "&") == 0);
}

int check_symbol(char *symbol, char **token_array, int token_arr_count){

	int index = 0;

	while(index < token_arr_count){
		if(strcmp(token_array[index], symbol) == 0){
			break;
		}

		index = index + 1;
	}

	if(index < token_arr_count){
		return index;
	}

	
	return 0;
}

int check_pipe(){
	int index;
	char pipe[] = "|\0";
	if((index = check_symbol(pipe, tokens, token_count)) > 0){
		return index;
	}

	return -1;
}

int check_inSymbol(){
	int index;
	char in[] = "<\0";
	if((index = check_symbol(in, tokens, token_count)) > 0){
		return index;
	}

	return -1;
}

int check_outSymbol(){
	int index;
	char out[] = ">\0";
	if((index = check_symbol(out, tokens, token_count)) > 0){
		return index;
	}

	return -1;
}

int handle_io(){
	int in_symbol_index, out_symbol_index, ret, status, case_var;
	pid_t pid;
	int fds[2];

	if((pid = fork()) < 0){
		perror("fork failed");
	}

	if(pid == 0){
		if(IO){
			case_var = 0;
			in_symbol_index = check_inSymbol();
			out_symbol_index = check_outSymbol();
			fds[0] = open(tokens[(in_symbol_index + 1)], O_RDONLY);
			fds[1] = open(tokens[(out_symbol_index + 1)], O_WRONLY);
			dup2(fds[0], STDIN_FILENO);
			dup2(fds[1], STDOUT_FILENO);
			close(fds[0]);
			close(fds[1]);
		}
		if(INPUT){
			case_var = 1;
			in_symbol_index = check_inSymbol();
			fds[0] = open(tokens[(in_symbol_index + 1)], O_RDONLY);
			dup2(fds[0], STDIN_FILENO);
			close(fds[0]);
		}
		if(OUTPUT){
			case_var = 2;
			out_symbol_index = check_outSymbol();
			fds[1] = open(tokens[(out_symbol_index + 1)], O_WRONLY);
			dup2(fds[1], STDOUT_FILENO);
			close(fds[1]);
		}
		char **command = malloc(sizeof(char *) * 10);
		int i = 0, index = 0;

		if(case_var == 0 || case_var == 1){
			index = in_symbol_index;
		}
		else{
			index = out_symbol_index;
		}

		while(i < index){
			command[i] = tokens[i];
			i = i + 1;
		}
		execvp(command[0], command);
		perror("exec failed in file io redirection");
		exit(1);
	}
	else {
		if(is_background()) {
			// record bg task
			int next_free = next_free_bg();
			assert(next_free != -1); // we should have checked earlier
			bgtask[next_free].pid = pid;
			bgtask[next_free].status = RUNNING;
			bg_count++;
		} else {
			// parent waits on foreground command
			ret = waitpid(pid, &status, 0);
			if( ret < 0) {
				perror("error waiting for child:");
				return ERROR;
			}
		}
	}
	return 0;
}

void prepare_pipe_commands(){
	
	int  token_counter = 0;
	command_index = 0;

	commands = malloc(sizeof(char **) * 10);
	commands[0] = malloc(sizeof(char **) * 10);

	int i = 0;
	
	// preparing the commands such that they are indexed
	//example for [ ls -l | grep a | wc -l ]: commands[0] -> ls -l   commands[1]-> grep a    commands[2] -> wc -l 
	while(tokens[i] != NULL){
		if(strcmp(tokens[i],"|\0") == 0){
			commands[command_index][token_counter] = NULL;
			command_index = command_index + 1;
			commands[command_index] = malloc(sizeof(char **) * 10);
			token_counter = 0;
			i = i + 1;
			continue;
		}

		commands[command_index][token_counter] = tokens[i];
		token_counter = token_counter + 1;
		i = i + 1;
	}
}

void reset_commands(){
	while(command_index > -1){
		free(commands[command_index]);
		command_index = command_index - 1;
	}
	free(commands);
	commands = NULL;
}

int handle_pipe(int command_seq_no, int fd_in){
	
	pid_t pid;
	int fds[2];
	int ret, status;

	if(command_seq_no == 0){
		if(commands != NULL){
			reset_commands();
		}
		prepare_pipe_commands();
	}

	if(command_seq_no == command_index){ //for the last command
		
		// fork last child
		if((pid = fork()) < 0){
			perror("fork failed");
			return ERROR;
		}
		if(pid == 0){
			dup2(fd_in, STDIN_FILENO);
			//close(fd_in);
			execvp(commands[command_seq_no][0], commands[command_seq_no]);
			perror("Final command failed\n");
			return ERROR;
		}
		else if(pid > 0){
			if(is_background()) {
			// record bg task
				int next_free = next_free_bg();
				assert(next_free != -1); // we should have checked earlier
				bgtask[next_free].pid = pid;
				bgtask[next_free].status = RUNNING;
				bg_count++; 
			} 
		}
		
	}
	else{
		//setup pipe
		if(pipe(fds) < 0){
			perror("pipe setup failed\n");
		}

		//fork child
		if((pid = fork()) < 0){
			perror("fork failed");
		}

		// child executes here
		if(pid == 0){
			if(fd_in != STDIN_FILENO){     // for commands other than the first command
				dup2(fd_in, STDIN_FILENO);
				close(fd_in);
			}
			dup2(fds[1], STDOUT_FILENO);
			close(fds[1]);
			close(fds[0]);
			execvp(commands[command_seq_no][0], commands[command_seq_no]);
			perror("Exec failed on initial or middle commands\n");
			exit(1);
		}

		// parent executes here
		if(pid > 0){
			ret = waitpid(pid, &status, 0);            // new addition
			printf("Return value: %d\n",ret);            //
			if( ret < 0) {                             // 
				perror("error waiting for child:");    //
				return ERROR;                          //
			}                                          //
			int new_command_seq_no = command_seq_no + 1;
			handle_pipe(new_command_seq_no, fds[0]);
			if(is_background()) {
			// record bg task
				int next_free = next_free_bg();
				assert(next_free != -1); // we should have checked earlier
				bgtask[next_free].pid = pid;
				bgtask[next_free].status = RUNNING;
				bg_count++;
			}
		}

	}

	return 0;
}

int run_command() 
{
	int pipe_posn, ret = 0, status;
	pid_t pid;
	// handle builtin commands first

	// exit
	if (strcmp( tokens[0], EXIT_STR ) == 0) {
		handle_exit();
		return EXIT_CMD;
	}

	// fg
	if (strcmp( tokens[0], FG_STR ) == 0) {
		handle_fg();
		return FG_CMD;
	}

	// listjobs
	if (strcmp( tokens[0], LISTJOBS_STR) == 0) {
		handle_listjobs();
		return FG_CMD;
	}

	// disallow more than MAX_BG_TASKS
	if (is_background() && (bg_count == MAX_BG_TASKS) ) {
		printf("No more background tasks allowed\n");
		return ERROR;
	}

	if((pipe_posn = check_pipe()) != -1){
		handle_pipe(0, STDIN_FILENO);
	}
	else if((check_inSymbol() > 0) || (check_outSymbol() > 0)){
		handle_io();
	}
	else{
		// fork child
		pid = fork();
		if( pid < 0) {
			perror("fork failed:");
			return ERROR;
		}

		if(pid==0) {
			// exec in child
			//remove & from arguments list if background command
			if(is_background()) tokens[token_count-1] = NULL;

			execvp(tokens[0], tokens);
			perror("exec faied:");
			exit(1);
		} else {
			if(is_background()) {
				// record bg task
				int next_free = next_free_bg();
				assert(next_free != -1); // we should have checked earlier
				bgtask[next_free].pid = pid;
				bgtask[next_free].status = RUNNING;
				bg_count++;
			} else {
				// parent waits on foreground command
				ret = waitpid(pid, &status, 0);
				if( ret < 0) {
					perror("error waiting for child:");
					return ERROR;
				}
			}
		}
	}
	return 0;
}

int main()
{
	int ret;

	initialize();

	do {
		ret = 0;

		printf("Terminal2> ");

		read_command();

		if(token_count > 0) ret = run_command();

	} while( (ret != EXIT_CMD) && (ret != ERROR) );

	return 0;
}

