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

sqlite3 *open_db()
{
    sqlite3* db;
    if (sqlite3_open("EMPLOYEE_DATA.db", &db) != SQLITE_OK)
    {
        fprintf(stderr, "Error opening file.");
        exit(1);
    }
    return db;
} 

void init_db()
{
    sqlite3 *db = open_db();
    sqlite3_exec(db, 
            "CREATE TABLE IF NOT EXISTS users ("
                "name TEXT,"
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "position TEXT,"
                "is_cleared INTEGER"
                ");",
                0,0,0
            );

    printf("Database INIT success. Nothing to report here modafucka");
}
int CREATE_ENTRY(char *USER_FULL_NAME, int ID_NUM, int COMPANY_POSITION, int CLEARANCE_STATUS)
{
    init_db();
    sqlite3 *db = open_db();
    //prepare arguments in parameterized queries


}
