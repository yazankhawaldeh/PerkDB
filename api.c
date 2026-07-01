#include <stdlib.h>
#include <stdio.h>
#include "sqlite-amalgamation-3530200/sqlite3.h"


// CRUD API FOR WRITING TO DATABASE
#define WRITE_OK 0
#define WRITE_ERR 1
#define READ_OK 0
#define READ_ERR 1

/*
 * DATABSE SCHEMA
 * User = name TEXT, id INTEGER PRIMARY KEY AUTOINCREMENT, position TEXT, 
 */

sqlite3 *open_db(char *FILE_NAME)
{
    sqlite3* db;
    if (sqlite3_open(FILE_NAME, &db) != SQLITE_OK)
    {
        fprintf(stderr, "Error opening file.");
        exit(1);
    }
    return db;
} 

int CREATE_TABLE(char *FILE_NAME, char* TABLE_NAME, int NUM_COLUMNS, char** COLUMN_NAMES)
{
    sqlite3 *db = open_db(FILE_NAME);


    //prepare arguments in parameterized queries
    //


}


