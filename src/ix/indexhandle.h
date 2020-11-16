#pragma once
#include "../filesystem/fs.h"
#include "utils.h"
#include "../rm/record.h"
#include <cstring>
#include <cstdio>
#include <vector>


class IX_IndexHandle {
    public:
        int fileID;
        FileManager *fm;
        BufPageManager *bpm;

        IX_FileConfig fileConfig;

    
        IX_IndexHandle(FileManager *fm, BufPageManager *bpm, int fileID) {
            this->fileID = fileID;
            this->fm = fm;
            this->bpm = bpm;

            int tmp_index;
            BufType data = bpm->getPage(fileID, 0, tmp_index);
            memcpy(&fileConfig, data, sizeof(IX_FileConfig));
            
            bpm->access(tmp_index);
        }
        ~IX_IndexHandle() {}

        bool insertEntry(void *pData, RID rid) {
            int curID = fileConfig.rootNode;
            
            IX_BPlusTreeNode *fake = nullptr;
            bool ret = recurInsertEntry(curID, pData, rid, fake);
            if(fake) {
                // fake->debug();
                forceWrite(fake);
                delete fake;
            }
            updateFileConfig();
            return ret;
        }

        bool deleteEntry(void *pData, RID rid) {
            int rootID = fileConfig.rootNode;

            IX_BPlusTreeNode *fake = nullptr;
            bool ret = recurDeleteEntry(rootID, pData, rid, fake);
            if(fake) {
                forceWrite(fake);
                delete fake;
            }
            updateFileConfig();
            
            return ret;
        }

        bool searchFirstEntry(void *pData, int &retID, int &index) {
            int rootID = fileConfig.rootNode;
            return recurSearchEntryFirst(rootID, pData, retID, index);
        }

        bool searchLastEntry(void *pData, int &retID, int &index) {
            int rootID = fileConfig.rootNode;
            return recurSearchEntry(rootID, pData, retID, index);
        }


        bool searchEntry(void *pData, vector<RID>& vec) {
            int rootID = fileConfig.rootNode;
            int attrLen = fileConfig.attrLength;
            AttrType attrType = fileConfig.attrType;

            int index = -1;
            int nodeID = -1;
            bool ret = recurSearchEntry(rootID, pData, nodeID, index);
            if(!ret) return false;
            IX_BPlusTreeNode* node = new IX_BPlusTreeNode();
            convertPageToNode(nodeID, node);
            while(true) {
                for(int i = index; i >= 0; --i) {
                    if(compareAttr(pData, node->getIthKeyPointer(i, attrLen), attrType, attrLen) == 0) {
                        vec.push_back(RID(node->getIthPage(i), node->getIthSlot(i)));
                    } else {
                        delete node;
                        return true;
                    }
                }
                int prevID = node->prevNode;
                if(prevID > 0) {
                    convertPageToNode(prevID, node);
                    index = node->curNum - 1;
                } else {
                    delete node;
                    return true;
                }
            }
        }





        void convertPageToNode(int pageID, IX_BPlusTreeNode* &node) {
            if(!node) node = new IX_BPlusTreeNode();
            
            int index;
            BufType buf = bpm->getPage(fileID, pageID, index);
            
            
            memcpy(node, buf, fileConfig.treeNodeInfoSize);

            
            node->rids = buf + fileConfig.treeNodeInfoSize;
            node->keys = buf + fileConfig.treeNodeInfoSize + fileConfig.maxRidSize;

            node->selfID = pageID;

        }



        void debug(int id) {
            IX_BPlusTreeNode *node = new IX_BPlusTreeNode();
            convertPageToNode(id, node);
            node->debug();
            if(!node->isLeafNode) {
                for(int i = 0; i < node->curNum; ++i) {
                    debug(node->getIthPage(i));
                }
            }

            delete node;
            return;
        }

        void iterLeaves(vector<int>&key, vector<RID>& vec) {
            int rootID = fileConfig.rootNode;
            IX_BPlusTreeNode *node = new IX_BPlusTreeNode();
            convertPageToNode(rootID, node);
            while(!node->isLeafNode) {
                int id = node->getIthPage(0);
                convertPageToNode(id, node);
            }
            while(true) {
                for(int i = 0; i < node->curNum; ++i) {
                    key.push_back(*(node->getIthKeyPointer(i, fileConfig.attrLength)));
                    vec.push_back(RID(node->getIthPage(i), node->getIthSlot(i)));
                }
                if(node->nextNode > 0) {
                    int next = node->nextNode;
                    convertPageToNode(next, node);
                } else {
                    return;
                }
            }
        }

        void forceWrite(IX_BPlusTreeNode *node) {
            if(node->selfID <= 0) return;

            // printf("force %d %d\n", node->selfID, node->curNum);
            int index;
            BufType buf = bpm->getPage(fileID, node->selfID, index);
            memcpy(buf, node, fileConfig.treeNodeInfoSize);
            bpm->markDirty(index);
        }

        void getFirstEntry(IX_BPlusTreeNode* &node) {
            int rootID = fileConfig.rootNode;
            if(!node)
                node = new IX_BPlusTreeNode();
            convertPageToNode(rootID, node);
            while(!node->isLeafNode) {
                int id = node->getIthPage(0);
                convertPageToNode(id, node);
            }
        }


    private:

        bool recurInsertEntry(int nodeID, void *pData, RID rid, IX_BPlusTreeNode* &parentNode) {
            int m = fileConfig.maxKeyNum;
            int attrLen = fileConfig.attrLength;
            AttrType attrType = fileConfig.attrType;

            IX_BPlusTreeNode* curNode = new IX_BPlusTreeNode();
            convertPageToNode(nodeID, curNode);

            bool ret = false;

            if(!curNode->isLeafNode) {
                for(int i = curNode->curNum - 1; i >= 0; --i) {
                    void* tmp_key = curNode->getIthKeyPointer(i, attrLen);
                    if(i == 0 || compareAttr(pData, tmp_key, attrType, attrLen) >= 0) {
                        int new_id = curNode->rids[2 * i];
                        ret = recurInsertEntry(new_id, pData, rid, curNode);
                        break;
                    }
                }
            } else {
                int i;
                for(i = curNode->curNum; i > 0; --i) {
                    void* tmp_key = curNode->getIthKeyPointer(i - 1, attrLen);
                    if(compareAttr(pData, tmp_key, attrType, attrLen) >= 0) 
                        break;
                    memcpy(curNode->getIthKeyPointer(i, attrLen), curNode->getIthKeyPointer(i - 1, attrLen), attrLen);
                    curNode->setRID(i, curNode->getIthPage(i - 1), curNode->getIthSlot(i - 1));
                }

                // printf("insert %d, i = %d\n", *((int*)pData), i);

                memcpy(curNode->getIthKeyPointer(i, attrLen), pData, attrLen);
                curNode->setRID(i, rid);


                curNode->curNum += 1;
                ret = true;
            }



            if(curNode->curNum >= fileConfig.maxKeyNum) {

                // IX_BPlusTreeNode* parentNode = NULL;

                if(curNode->parentNode <= 0) {
                    int parent = ++fileConfig.curNodeNum;
                    convertPageToNode(parent, parentNode);
                    fileConfig.rootNode = parent;

                    parentNode->init(false, -1, -1, -1);
                    memcpy(parentNode->keys, curNode->keys, attrLen);
                    parentNode->rids[0] = nodeID;
                    parentNode->curNum = 1;
                    
                    curNode->parentNode = parent;

                    // printf("\nmake new parent %d %d\n", curNode->selfID, curNode->curNum);
                    
                }



                IX_BPlusTreeNode* newNode = splitNode(curNode, attrLen);
                
                // printf("\ndone split\n");

                // if(!parentNode) parentNode = convertPageToNode(curNode->parentNode);
                int curWhichChild = whichChild(nodeID, parentNode);

                if(curWhichChild == -1) {
                    printf("error\n");
                }


                for(int i = parentNode->curNum; i > curWhichChild + 1; --i) {
                    memcpy(parentNode->getIthKeyPointer(i, attrLen), parentNode->getIthKeyPointer(i - 1, attrLen), attrLen);
                    parentNode->setRID(i, parentNode->getIthPage(i - 1), parentNode->getIthSlot(i - 1));
                }

                parentNode->curNum += 1;
                // printf("add %d %d\n\n", parentNode->selfID, parentNode->curNum);

                memcpy(parentNode->getIthKeyPointer(curWhichChild, attrLen), curNode->keys, attrLen);
                memcpy(parentNode->getIthKeyPointer(curWhichChild + 1, attrLen), newNode->keys, attrLen);

                parentNode->setRID(curWhichChild + 1, newNode->selfID, -1);

                
                forceWrite(newNode);

               

                delete newNode;
                // delete parentNode;

            }

            forceWrite(curNode);
            
            delete curNode;



            return ret;
        }

        IX_BPlusTreeNode* splitNode(IX_BPlusTreeNode* curNode, int attrLen) {
            int newNodeID = ++fileConfig.curNodeNum;
            int curID = curNode->selfID;
            
            IX_BPlusTreeNode* newNode = new IX_BPlusTreeNode();
            convertPageToNode(newNodeID, newNode);
            newNode->init(curNode->isLeafNode, curID, curNode->nextNode, curNode->parentNode);

            int m = curNode->curNum / 2;

            // printf("\nm = %d\n", newNode->parentNode);            
            newNode->curNum = curNode->curNum - m;

            for(int i = 0; i < newNode->curNum; ++i) {
                // printf("move %d\n", *(curNode->getIthKeyPointer(i + m, attrLen)));
                memcpy(newNode->getIthKeyPointer(i, attrLen), curNode->getIthKeyPointer(i + m, attrLen), attrLen);
                newNode->setRID(i, curNode->getIthPage(i + m), curNode->getIthSlot(i + m));

                // printf("move %d\n", *(newNode->getIthKeyPointer(i, attrLen)));
            }



            curNode->curNum = m;
            curNode->nextNode = newNodeID;

            // printf("\ndone trans\n");

            if(!newNode->isLeafNode) {
                for(int i = 0; i < newNode->curNum; ++i) {
                    int childPage = newNode->getIthPage(i);
                    if(childPage > 0) {
                        modifyParent(childPage, newNode->selfID);
                    }
                }
            } else {
                if(newNode->nextNode > 0) {
                    modifyPrev(newNode->nextNode, newNode->selfID);
                }
            }


            return newNode;


        }


        

        bool recurDeleteEntry(int nodeID, void *pData, RID rid, IX_BPlusTreeNode* &parentNode) {
            int attrLen = fileConfig.attrLength;
            AttrType attrType = fileConfig.attrType;

            IX_BPlusTreeNode* curNode = new IX_BPlusTreeNode();
            convertPageToNode(nodeID, curNode);

            bool ret = false;

            if(!curNode->isLeafNode) {
                int start = 0;
                for(int i = 0; i < curNode->curNum; ++i) {
                    start = i;
                    if(i == curNode->curNum-1 || compareAttr(pData, curNode->getIthKeyPointer(i+1, attrLen), attrType, attrLen) < 0)
                        break;
                }


                for(int i = curNode->curNum - 1; i >= start; --i) {
                    void* tmp_key = curNode->getIthKeyPointer(i, attrLen);
                    if(i == start || compareAttr(pData, tmp_key, attrType, attrLen) >= 0) {
                        int new_id = curNode->rids[2 * i];
                        ret |= recurDeleteEntry(new_id, pData, rid, curNode);
                        if(ret) break;
                    }
                }
            } else {
                int id = -1;
                for(int i = 0; i < curNode->curNum; ++i) {
                    if(compareAttr(pData, curNode->getIthKeyPointer(i, attrLen), attrType, attrLen) == 0 && rid.equal(curNode->getIthPage(i), curNode->getIthSlot(i))) {
                        id = i;
                        break;
                    }
                }

                if(id != -1) {
                    curNode->curNum -= 1;
                    for(int i = id; i < curNode->curNum; ++i) {
                        memcpy(curNode->getIthKeyPointer(i, attrLen), curNode->getIthKeyPointer(i + 1, attrLen), attrLen);
                        curNode->setRID(i, curNode->getIthPage(i + 1), curNode->getIthSlot(i + 1));
                    }
                    ret = true;

                } else {
                    delete curNode;
                    return false;
                }

            }

            if(curNode->curNum == 0) {
                if(curNode->isLeafNode) {
                    if(curNode->prevNode > 0)
                        modifyNext(curNode->prevNode, curNode->nextNode);
                    if(curNode->nextNode > 0)
                        modifyPrev(curNode->nextNode, curNode->prevNode);
                }

                if(nodeID == fileConfig.rootNode) return ret;

                int curWhichChild = whichChild(nodeID, parentNode);
                
                if(curWhichChild == -1) {
                    printf("error");
                }

                parentNode->curNum -= 1;
                for(int i = curWhichChild; i < parentNode->curNum; ++i) {
                    memcpy(curNode->getIthKeyPointer(i, attrLen), curNode->getIthKeyPointer(i + 1, attrLen), attrLen);
                    curNode->setRID(i, curNode->getIthPage(i + 1), curNode->getIthSlot(i + 1));
                }


            }

            forceWrite(curNode);
            delete curNode;
            
            return ret;
        }

        bool recurSearchEntryFirst(int nodeID, void *pData, int &retID, int &index) {
            int startID, startIndex;
            bool ret = recurSearchEntry(nodeID, pData, startID, startIndex);
            if(!ret) return ret;

            IX_BPlusTreeNode* curNode = new IX_BPlusTreeNode();
            convertPageToNode(startID, curNode);
            retID = startID;
            index = startIndex;
            while(true) {
                for(int i = startIndex; i >= 0; --i) {
                    if(compareAttr(pData, curNode->getIthKeyPointer(i, fileConfig.attrLength), fileConfig.attrType, fileConfig.attrLength) == 0) {
                        index = i;
                    } else {
                        delete curNode;
                        return true;
                    }
                }
                int prev = curNode->prevNode;
                if(prev > 0) {
                    convertPageToNode(prev, curNode);
                    startIndex = curNode->curNum - 1;
                } else {
                    delete curNode;
                    return true;
                }
            }
        }

        bool recurSearchEntry(int nodeID, void *pData, int &retID, int &index) {
            int attrLen = fileConfig.attrLength;
            AttrType attrType = fileConfig.attrType;

            IX_BPlusTreeNode* curNode = new IX_BPlusTreeNode();
            convertPageToNode(nodeID, curNode);

            bool ret = false;

            if(!curNode->isLeafNode) {
                
                for(int i = curNode->curNum - 1; i >= 0; --i) {
                    void* tmp_key = curNode->getIthKeyPointer(i, attrLen);
                    if(i == 0 || compareAttr(pData, tmp_key, attrType, attrLen) >= 0) {
                        int new_id = curNode->rids[2 * i];
                        ret = recurSearchEntry(new_id, pData, retID, index);
                        delete curNode;
                        return ret;
                    }
                }
            } else {
                for(int i = curNode->curNum - 1; i >= 0; --i) {
                    if(compareAttr(pData, curNode->getIthKeyPointer(i, attrLen), attrType, attrLen) == 0) {
                        index = i;
                        retID = nodeID;
                        delete curNode;
                        return true;
                    }
                }

                
                delete curNode;
                return false;
                

            }
        }

        void modifyParent(int id, int parent) {
            IX_BPlusTreeNode *tmp = new IX_BPlusTreeNode();
            convertPageToNode(id, tmp);
            tmp->parentNode = parent;
            forceWrite(tmp);
            delete tmp;
        }

        void modifyPrev(int id, int prev){
            IX_BPlusTreeNode *tmp = new IX_BPlusTreeNode();
            convertPageToNode(id, tmp);
            tmp->prevNode = prev;
            forceWrite(tmp);
            delete tmp;
        }

        void modifyNext(int id, int next) {
            IX_BPlusTreeNode *tmp = new IX_BPlusTreeNode();
            convertPageToNode(id, tmp);
            tmp->nextNode = next;
            forceWrite(tmp);
            delete tmp;
        }

        

        void updateFileConfig() {
            int index;
            BufType buf = bpm->getPage(fileID, 0, index);
            memcpy(buf, &(fileConfig), sizeof(IX_FileConfig));
            bpm->markDirty(index); 
        }

        int whichChild(int curID, IX_BPlusTreeNode *parentNode) {
            if(parentNode->isLeafNode) return -1;
            for(int i = 0; i < parentNode->curNum; ++i) {
                if(parentNode->getIthPage(i) == curID) return i;
            }
            return -1;
        }



        
        
};