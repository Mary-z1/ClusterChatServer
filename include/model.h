#ifndef MODEL_H
#define MODEL_H

#include "db.h"
#include <string>
#include <vector>
#include <utility>

// ========== 用户数据模型 ==========
class UserModel {
public:
    // 插入新用户，返回自增 id（失败返回 -1）
    static int insert(MySQL* mysql, const std::string& name,
                      const std::string& password);

    // 按用户名查询，返回 (id, password)。不存在则 id=0
    static std::pair<int, std::string> queryByName(MySQL* mysql,
                                                    const std::string& name);

    // 更新在线状态
    static bool updateState(MySQL* mysql, int id, const std::string& state);
};

// ========== 好友数据模型 ==========
class FriendModel {
public:
    // 添加好友关系（单向：userid 添加 friendid）
    static bool add(MySQL* mysql, int userid, int friendid);

    // 查询用户的所有好友 id
    static std::vector<int> queryAll(MySQL* mysql, int userid);
};

// ========== 群组数据模型 ==========
class GroupModel {
public:
    // 创建群组，返回 groupid（失败返回 -1）
    static int create(MySQL* mysql, const std::string& name, int creatorId);

    // 添加群成员
    static bool addMember(MySQL* mysql, int groupid, int userid,
                          const std::string& role);

    // 查询群所有成员 id
    static std::vector<int> queryMembers(MySQL* mysql, int groupid);

    // 查询用户加入的所有群，返回 (groupid, groupname) 列表
    static std::vector<std::pair<int, std::string>>
    queryUserGroups(MySQL* mysql, int userid);
};

#endif