//  Powiter
//
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 *Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 *contact: immarespond at gmail dot com
 *
 */

#include "AbstractCache.h"

#include <sstream>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QXmlStreamReader>
#include <QtCore/QXmlStreamWriter>

#if QT_VERSION < 0x050000
#include <QtGui/QDesktopServices>
#else
#include <QStandardPaths>
#endif

#include "Global/Macros.h"
#include "Engine/MemoryFile.h"

using namespace std;
using namespace Powiter;

#if QT_VERSION < 0x050000
static bool removeRecursively(const QString & dirName)
{
    bool result = false;
    QDir dir(dirName);
    
    if (dir.exists(dirName)) {
        Q_FOREACH(QFileInfo info, dir.entryInfoList(QDir::NoDotAndDotDot | QDir::System | QDir::Hidden  | QDir::AllDirs | QDir::Files, QDir::DirsFirst)) {
            if (info.isDir()) {
                result = removeRecursively(info.absoluteFilePath());
            }
            else {
                result = QFile::remove(info.absoluteFilePath());
            }
            
            if (!result) {
                return result;
            }
        }
        result = dir.rmdir(dirName);
    }
    return result;
}
#endif

MemoryMappedEntry::MemoryMappedEntry()
: _path()
, _mappedFile(0)
{
}

bool MemoryMappedEntry::allocate(U64 byteCount,const char* path) {
    assert(!_lock.tryLock());
    assert(path);
#ifdef POWITER_DEBUG
    if(QFile::exists(path)){
        cout << "WARNING: A file with the same name already exists : " << path
        << " (If displayed on startup ignore it, this is a debug message "
        << " , if not,it probably means your hashing function does not scatter"
        << " the entries efficiently enough)."<< endl;
    }
#endif
    try{
        _mappedFile = new MemoryFile(path,Powiter::if_exists_keep_if_dont_exists_create);
    }catch(const char* str){
        cout << str << endl;
        deallocate();
        if(QFile::exists(path)){
            QFile::remove(path);
        }
        return false;
    }
    _mappedFile->resize(byteCount);
    _size += _mappedFile->size();
    string newPath(path);
    if(_path.empty()) {
        _path.append(path);
    } else if(_path != newPath) {
        _path = newPath;
    }
    return true;
}

void MemoryMappedEntry::deallocate() {
    assert(!_lock.tryLock());
    if(_mappedFile){
        delete _mappedFile;
    }
    _mappedFile = 0;
}

bool MemoryMappedEntry::reOpen(){
    assert(!_lock.tryLock());
    if(_path.empty()) return false;
    try{
        _mappedFile = new MemoryFile(_path.c_str(),Powiter::if_exists_keep_if_dont_exists_fail);
    }catch(const char* str){
        deallocate();
        if(QFile::exists(_path.c_str())){
            QFile::remove(_path.c_str());
        }
        cout << str << endl;
        return false;
    }
    return true;
}

MemoryMappedEntry::~MemoryMappedEntry() {
    deallocate();
}

#if 0 // unused
InMemoryEntry::InMemoryEntry()
:_data(0)
{
}

bool InMemoryEntry::allocate(U64 byteCount,const char* path) {
    assert(!_lock.tryLock());
    Q_UNUSED(path);
    _data = (char*)malloc(byteCount);
    if(!_data) return false;
    return true;
}

void InMemoryEntry::deallocate() {
    assert(!_lock.tryLock());
    free(_data);
    _data = 0;
}
InMemoryEntry::~InMemoryEntry(){
    deallocate();
}
#endif


AbstractCacheHelper::AbstractCacheHelper():_maximumCacheSize(0),_size(0){
    
}
AbstractCacheHelper::~AbstractCacheHelper(){
    QMutexLocker locker(&_lock);
    for(CacheIterator it = _cache.begin() ; it!=_cache.end() ; ++it) {
        CacheEntry* entry = getValueFromIterator(it);
        entry->lock();
        delete entry;
    }
    _cache.clear();
    _size = 0;
    
}

void AbstractCacheHelper::clear() {
    QMutexLocker locker(&_lock);
    std::vector<std::pair<U64,CacheEntry*> > backUp;
    for(CacheIterator it = _cache.begin() ; it!=_cache.end() ; ++it) {
        CacheEntry* entry = getValueFromIterator(it);
        entry->lock();
        assert(entry);
        if (entry->isMemoryMappedEntry()) {
            MemoryMappedEntry* mmapEntry = dynamic_cast<MemoryMappedEntry*>(entry);
            if (mmapEntry) {
                if(QFile::exists(mmapEntry->path().c_str())){
                    QFile::remove(mmapEntry->path().c_str());
                }
            }
        }
        _size -= entry->size();
        if (entry->isRemovable()) {
            delete entry;
        } else {
            backUp.push_back(make_pair(it->first,it->second));
        }
    }
    _cache.clear();
    for (unsigned int i = 0; i < backUp.size(); ++i) {
        add(backUp[i].first, backUp[i].second);
        backUp[i].second->unlock();
    }
}

void AbstractCacheHelper::erase(CacheIterator it){
    _cache.erase(it);
}

/*Returns an iterator to the cache. If found it points
 to a valid cache entry, otherwise it points to to end.*/
// on output, the cache entry is locked
CacheEntry* AbstractCacheHelper::getCacheEntry(U64 key)  {
    QMutexLocker locker(&_lock);
    CacheEntry* ret;
    ret = _cache(key);
    // do NOT unlock the cache (_lock) here, or ret may become an invalid cache entry
    if(ret) {
        // ret is not locked (freshly grabbed from the map)
        ret->lock();
        ret->addReference();
    }
    return ret;
}


// on input, entry must be locked, on output it is still locked
bool AbstractCacheHelper::add(U64 key,CacheEntry* entry){
    std::pair<U64,CacheEntry*> evicted;
    bool evict = false;
    {
        QMutexLocker locker(&_lock);
        if (_size >= _maximumCacheSize) {
            evict = true;
        }
        _size += entry->size();
        evicted = _cache.insert(key,entry,evict);
        if (evicted.second) {
            evicted.second->lock();
            _size -= evicted.second->size();
            evicted.second->unlock();
        }
    }
    if (evicted.second) {
        /*while we removed an entry from the cache that must not be removed, we insert it again.
         If all the entries in the cache cannot be removed (in theory it should never happen), the
         last one will be evicted.*/
        evicted.second->lock();
        while(!evicted.second->isRemovable()) {
            QMutexLocker locker(&_lock);
            evicted.second->unlock();
            evicted = _cache.insert(key,entry,true);
            assert(evicted.second);
            evicted.second->lock();
            _size -= evicted.second->size();
            evicted.second->unlock();
        }
        
        /*if it is a memorymapped entry, remove the backing file in the meantime*/
        if (evicted.second->isMemoryMappedEntry()) {
            MemoryMappedEntry* mmEntry = dynamic_cast<MemoryMappedEntry*>(evicted.second);
            if (mmEntry) {
                assert(mmEntry->getMappedFile());
                std::string path = mmEntry->getMappedFile()->path();
                QFile::remove(path.c_str());
            }
        }
        delete evicted.second;
    }
    return evict;
}

bool AbstractMemoryCache::add(U64 key,CacheEntry* entry){
    assert(!entry->isMemoryMappedEntry());
    return AbstractCacheHelper::add(key, entry);
}

AbstractDiskCache::AbstractDiskCache(double inMemoryUsage)
: _inMemorySize(0)
, _maximumInMemorySize(inMemoryUsage)
, _inMemoryPortion()
{
}

// on input, entry must be locked, on output it is still locked
bool AbstractDiskCache::add(U64 key, CacheEntry* entry) {
    MemoryMappedEntry* mmEntry = dynamic_cast<MemoryMappedEntry*>(entry);
    assert(mmEntry);
    bool mustEvictFromMemory = false;
    std::pair<U64,CacheEntry*> evicted;
    {
        QMutexLocker locker(&_lock);
        if(_inMemorySize > _maximumInMemorySize*getMaximumSize()){
            mustEvictFromMemory = true;
        }
        _inMemorySize += mmEntry->size();
        evicted = _inMemoryPortion.insert(key, mmEntry, mustEvictFromMemory);
        if (evicted.second) {
            evicted.second->lock();
            _inMemorySize -= evicted.second->size();
        }
    }
    if (evicted.second) {
        if (evicted.second->isRemovable()) {
            /*switch the evicted entry from memory to the disk.*/
            MemoryMappedEntry* mmEvictedEntry = dynamic_cast<MemoryMappedEntry*>(evicted.second);
            assert(mmEvictedEntry);
            mmEvictedEntry->deallocate();
            bool ret = AbstractCacheHelper::add(evicted.first, evicted.second);
            evicted.second->unlock();
            return ret;
        } else {
            /*We risk an infinite loop here. It might happen if all entries in the cache cannot be removed.
             This is bad design if all entries in the cache cannot be removed, this means that they all live
             in memory and that you shouldn't use a disk cache for this purpose.*/
            add(evicted.first,evicted.second);
            evicted.second->unlock();
        }
    }
    
    return false;
}

// on output the CacheEntry is locked, and must be unlocked using CacheEntry::unlock()
CacheEntry* AbstractDiskCache::isInMemory(U64 key) {
    QMutexLocker locker(&_lock);
    CacheEntry* ret;
    ret = _inMemoryPortion(key);
    // do NOT unlock the cache (_lock) here, or ret may become an invalid cache entry
    if (ret) {
        // ret is not locked (freshly grabbed from the map)
        ret->lock();
        ret->addReference();
    }
    return ret;
}

void AbstractDiskCache::initializeSubDirectories(){
    assert(!_lock.tryLock()); // must be locked
    QDir cacheFolder(getCachePath());
    cacheFolder.mkpath(".");
    
    QStringList etr = cacheFolder.entryList();
    // if not 256 subdirs, we re-create the cache
    if (etr.size() < 256) {
        foreach(QString e, etr){
            cacheFolder.rmdir(e);
        }
    }
    for(U32 i = 0x00 ; i <= 0xF; ++i) {
        for(U32 j = 0x00 ; j <= 0xF ; ++j) {
            ostringstream oss;
            oss << hex <<  i;
            oss << hex << j ;
            string str = oss.str();
            cacheFolder.mkdir(str.c_str());
        }
    }
}

AbstractDiskCache::~AbstractDiskCache(){
}

void AbstractDiskCache::clearInMemoryCache() {
    QMutexLocker locker(&_lock);
    while (_inMemoryPortion.size() > 0) {
        std::pair<U64,CacheEntry*> evicted = _inMemoryPortion.evict();
        assert(evicted.second);
        evicted.second->lock();
        _inMemorySize -= evicted.second->size();
        locker.unlock(); // avoid deadlock
        if (evicted.second->isRemovable()) { // shouldn't we remove it anyay, even if it's not removable?
            MemoryMappedEntry* mmEntry = dynamic_cast<MemoryMappedEntry*>(evicted.second);
            assert(mmEntry);
            mmEntry->deallocate();
            AbstractCacheHelper::add(evicted.first, evicted.second);
        } else {
            add(evicted.first, evicted.second);
#warning "if evicted.second->isRemovable() is false, we go into an infinite loop here! Just do a simple project with reader+viewer"
        }
        evicted.second->unlock();
        locker.relock();
    }
}

void AbstractDiskCache::clearDiskCache(){
    clearInMemoryCache();
    clear();
    
    cleanUpDiskAndReset();
}

void AbstractDiskCache::save(){
    QString newCachePath(getCachePath());
    newCachePath.append(QDir::separator());
    newCachePath.append("restoreFile.powc");
    QFile restoreFile(newCachePath);
    restoreFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
    
    QXmlStreamWriter *writer = new QXmlStreamWriter(&restoreFile);
    writer->setAutoFormatting(true);
    writer->writeStartDocument();
    writer->writeStartElement("CacheEntries");
    writer->writeAttribute("Version",cacheVersion().c_str());
    /*clearing in-memory cache so only the on-disk portion has entries left.*/
    clearInMemoryCache();
    {
        QMutexLocker locker(&_lock);
        for(CacheIterator it = _cache.begin(); it!= _cache.end() ; ++it) {
            MemoryMappedEntry* mmEntry = dynamic_cast<MemoryMappedEntry*>(AbstractCache::getValueFromIterator(it));
            assert(mmEntry);
            mmEntry->lock();
            writer->writeStartElement("Entry");
            mmEntry->writeToXml(writer);
            writer->writeEndElement();
            mmEntry->unlock();
        }
    }
    writer->writeEndElement();
    writer->writeEndDocument();
    restoreFile.close();
    delete writer;
    
}

void AbstractDiskCache::restore(){
    QMutexLocker locker(&_lock);
    QString newCachePath(getCachePath());
    QString settingsFilePath(newCachePath);
    settingsFilePath.append(QDir::separator());
    settingsFilePath.append("restoreFile.powc");
    if(QFile::exists(settingsFilePath)){
        QDir directory(newCachePath);
        QStringList files = directory.entryList();
        
        
        /*Now counting actual data files in the cache*/
        /*check if there's 256 subfolders, otherwise reset cache.*/
        int count = 0; // -1 because of the restoreFile
        int subFolderCount = 0;
        for(int i =0 ; i< files.size() ;++i) {
            QString subFolder(newCachePath);
            subFolder.append(QDir::separator());
            subFolder.append(files[i]);
            if(subFolder.right(1) == QString(".") || subFolder.right(2) == QString("..")) continue;
            QDir d(subFolder);
            if(d.exists()){
                ++subFolderCount;
                QStringList items = d.entryList();
                for(int j = 0 ; j < items.size();++j) {
                    if(items[j] != QString(".") && items[j] != QString("..")) {
                        ++count;
                    }
                }
            }
        }
        if(subFolderCount<256){
            cout << cacheName() << " doesn't contain sub-folders indexed from 00 to FF. Reseting." << endl;
            cleanUpDiskAndReset();
        }
        std::vector<std::pair<U64,MemoryMappedEntry*> > entries;
        
        QFile restoreFile(settingsFilePath);
        restoreFile.open(QIODevice::ReadOnly);
        QXmlStreamReader *reader = new QXmlStreamReader(&restoreFile);
        while(!reader->atEnd() && !reader->hasError()){
            
            QXmlStreamReader::TokenType token = reader->readNext();
            
            /* If token is StartElement, we'll see if we can read it.*/
            if(token == QXmlStreamReader::StartElement) {
                if(reader->name() == "Entry"){
                    std::pair<U64,MemoryMappedEntry*> entry = entryFromXml(reader);
                    if(entry.second){
                        entry.second->lock();
                        entries.push_back(entry);
                    }else{
                        cout <<"WARNING: " <<  cacheName() << " failed to recover entry, discarding it." << endl;
                    }
                    
                }
                if(reader->name() == "CacheEntries"){
                    QXmlStreamAttributes versionAtts = reader->attributes();
                    bool shouldClean = false;
                    if(versionAtts.hasAttribute("Version")){
                        QString vers = versionAtts.value("Version").toString();
                        if(vers != cacheVersion().c_str()){
                            shouldClean = true;
                        }
                    }else{
                        shouldClean = true;
                    }
                    if(shouldClean){
                        
                        /*re-create the cache*/
                        restoreFile.remove();
                        locker.unlock(); // avoid deadlock
                        cleanUpDiskAndReset();
                        /*re-create settings file*/
                        //QFile _restoreFile(settingsFilePath); // error?
                        restoreFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
                        QTextStream outsetti(&restoreFile);
                        outsetti << cacheVersion().c_str() << endl;
                        restoreFile.close();
                        delete reader;
                        return;
                    }
                    
                }
            }
        }
        restoreFile.close();
        locker.unlock(); // avoid deadlock
        if ((U32)count == entries.size()) {
            for (U32 i = 0; i < entries.size(); ++i) {
                std::pair<U64,MemoryMappedEntry*>& entry = entries[i];
                add(entry.first,entry.second);
                entry.second->unlock();
            }
            
            /*cache is filled , debug*/
            // debug();
            
        } else {
            cout << cacheName() << ": The entries count in the restore file does not equal the number of actual data files.Reseting." << endl;
            cleanUpDiskAndReset();
        }
        
        /*now that we're done using it, clear it*/
        restoreFile.remove();
        delete reader;
    }else{ // restore file doesn't exist
        /*re-create cache*/
        
        QDir(getCachePath()).mkpath(".");
        locker.unlock(); // avoid deadlock
        cleanUpDiskAndReset();
    }
    
}

void AbstractDiskCache::cleanUpDiskAndReset(){
    QMutexLocker locker(&_lock);
    QString cachePath(getCachePath());
#   if QT_VERSION < 0x050000
    removeRecursively(cachePath);
#   else
    QDir dir(cachePath);
    if(dir.exists()){
        dir.removeRecursively();
    }
#endif
    initializeSubDirectories();
    
}

QString AbstractDiskCache::getCachePath(){
#if QT_VERSION < 0x050000
    QString cacheFolderName(QDesktopServices::storageLocation(QDesktopServices::CacheLocation));
#else
    QString cacheFolderName(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QDir::separator());
#endif
    cacheFolderName.append(QDir::separator());
    QString str(cacheFolderName);
    str.append(cacheName().c_str());
    return str;
}

void AbstractDiskCache::debug(){
    QMutexLocker locker(&_lock);
    cout << "====================DEBUGING: " << cacheName() << " =========" << endl;
    cout << "-------------------IN MEMORY ENTRIES-------------------" << endl;
    for (CacheIterator it = _inMemoryPortion.begin(); it!=_inMemoryPortion.end(); ++it) {
        MemoryMappedEntry* entry = dynamic_cast<MemoryMappedEntry*>(getValueFromIterator(it));
        assert(entry);
        entry->lock();
        cout << "[" << entry->path() << "] = " << entry->size() << " bytes. ";
        if(entry->getMappedFile()->data()){
            cout << "Entry has a valid ptr." << endl;
        }else{
            cout << "Entry has a NULL ptr." << endl;
        }
        entry->unlock();
        cout << "Key:" << it->first << endl;
    }
    cout <<" --------------------ON DISK ENTRIES---------------------" << endl;
    for (CacheIterator it = _cache.begin(); it!=_cache.end(); ++it) {
        MemoryMappedEntry* entry = dynamic_cast<MemoryMappedEntry*>(getValueFromIterator(it));
        assert(entry);
        entry->lock();
        cout << "[" << entry->path() << "] = " << entry->size() << " bytes. ";
        if(entry->getMappedFile()){
            cout << "Entry is still allocated!!" << endl;
        }else{
            cout << "Entry is normally deallocated." << endl;
        }
        entry->unlock();
        cout << "Key:" << it->first << endl;
    }
    cout << "Total entries count : " << _inMemoryPortion.size()+_cache.size() << endl;
    cout << "-===========END DEBUGGING: " << cacheName() << " ===========" << endl;
    
}

