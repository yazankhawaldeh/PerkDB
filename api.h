#ifndef API_H
#define API_H
#include "sqlite-amalgamation-3530200/sqlite3.h"

// This files defines the standard for the CRUD API function calls. In this file, you will find all function definitions along with the necessary arguments to call without error.

sqlite3 * open_db(char* FILE_NAME);


#endif 
