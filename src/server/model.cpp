#include "model.h"
#include "logger.h"
#include <cstdio>
#include <mysql/mysql.h>

// ==================== UserModel ====================

int UserModel::insert(MySQL* mysql, const std::string& name,
                       const std::string& password) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO User (name, password) VALUES ('%s', '%s')",
             mysql->escape(name).c_str(),
             mysql->escape(password).c_str());
    if (!mysql->update(sql)) return -1;
    return mysql_insert_id(mysql->getConnection());
}

std::pair<int, std::string> UserModel::queryByName(MySQL* mysql,
                                                    const std::string& name) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT id, password FROM User WHERE name='%s'",
             mysql->escape(name).c_str());
    MYSQL_RES* res = mysql->query(sql);
    if (!res) return {0, ""};

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return {0, ""}; }

    int id = std::stoi(row[0]);
    std::string pw = row[1];
    mysql_free_result(res);
    return {id, pw};
}

bool UserModel::updateState(MySQL* mysql, int id, const std::string& state) {
    char sql[128];
    snprintf(sql, sizeof(sql),
             "UPDATE User SET state='%s' WHERE id=%d",
             state.c_str(), id);
    return mysql->update(sql);
}

// ==================== FriendModel ====================

bool FriendModel::add(MySQL* mysql, int userid, int friendid) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT IGNORE INTO Friend (userid, friendid) VALUES (%d, %d), (%d, %d)",
             userid, friendid, friendid, userid);
    return mysql->update(sql);
}

std::vector<int> FriendModel::queryAll(MySQL* mysql, int userid) {
    std::vector<int> result;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT friendid FROM Friend WHERE userid=%d", userid);
    MYSQL_RES* res = mysql->query(sql);
    if (!res) return result;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
        result.push_back(std::stoi(row[0]));
    mysql_free_result(res);
    return result;
}

// ==================== GroupModel ====================

int GroupModel::create(MySQL* mysql, const std::string& name, int creatorId) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO GroupInfo (name, creator_id) VALUES ('%s', %d)",
             mysql->escape(name).c_str(), creatorId);
    if (!mysql->update(sql)) return -1;
    return mysql_insert_id(mysql->getConnection());
}

bool GroupModel::addMember(MySQL* mysql, int groupid, int userid,
                           const std::string& role) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO GroupUser (groupid, userid, role) VALUES (%d, %d, '%s')",
             groupid, userid, role.c_str());
    return mysql->update(sql);
}

std::vector<int> GroupModel::queryMembers(MySQL* mysql, int groupid) {
    std::vector<int> result;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT userid FROM GroupUser WHERE groupid=%d", groupid);
    MYSQL_RES* res = mysql->query(sql);
    if (!res) return result;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
        result.push_back(std::stoi(row[0]));
    mysql_free_result(res);
    return result;
}

std::vector<std::pair<int, std::string>>
GroupModel::queryUserGroups(MySQL* mysql, int userid) {
    std::vector<std::pair<int, std::string>> result;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT g.id, g.name FROM GroupInfo g "
             "INNER JOIN GroupUser gu ON g.id=gu.groupid "
             "WHERE gu.userid=%d", userid);
    MYSQL_RES* res = mysql->query(sql);
    if (!res) return result;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
        result.emplace_back(std::stoi(row[0]), row[1]);
    mysql_free_result(res);
    return result;
}