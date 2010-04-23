#include "collectionscanner.h"
#include "database.h"
#include "model/track.h"
#include "datautils.h"

// #include "freedesktopmime/freedesktopmime.h"
// #include "phonon/backendcapabilities.h"

CollectionScanner::CollectionScanner() :
        working(false),
        incremental(false),
        maxQueueSize(0),
        lastUpdate(0) { }

void CollectionScanner::run() {

    qDebug() << "CollectionScanner::run()" << rootDirectory;

    if (working) {
        emit error("A scanning task is already running");
        return;
    }
    working = true;

    lastUpdate = Database::instance().lastUpdate();

    // now scan the files
    scanDirectory(rootDirectory);

    maxQueueSize = fileQueue.size();

    // Start transaction
    // http://web.utk.edu/~jplyon/sqlite/SQLite_optimization_FAQ.html#transactions
    // Database::instance().getConnection().transaction();

    foreach(QFileInfo fileInfo, fileQueue) {
        // qDebug() << "Processing " << fileInfo.absoluteFilePath();

        // parse metadata with TagLib
        TagLib::FileRef fileref(fileInfo.absoluteFilePath().toUtf8());

        // if taglib cannot parse the file, drop it
        if (fileref.isNull()) {
            fileQueue.removeAll(fileInfo);
            continue;
        }

        // Ok this is an interesting file
        // This is not perfect as it will try to parse invalid files over and over
        // TODO Add a table for invalid files with a hash from mdate and size

        // This object will experience an incredible adventure,
        // facing countless perils and hopefully reaching to its final destination
        FileInfo *file = new FileInfo();
        file->setFileInfo(fileInfo);

        // Copy TagLib::FileRef in our Tags class.
        // TagLib::FileRef keeps files open and we would quickly reach the max open files limit
        Tags *tags = new Tags();
        tags->title = QString::fromStdString(fileref.tag()->title().to8Bit(true));
        tags->artist = QString::fromStdString(fileref.tag()->artist().to8Bit(true));
        tags->album = QString::fromStdString(fileref.tag()->album().to8Bit(true));
        tags->track = fileref.tag()->track();
        tags->year = fileref.tag()->year();
        file->setTags(tags);

        // get data from the internet
        giveThisFileAnArtist(file);
    }

    if (incremental) {
        // clean db from stale data: non-existing files
        cleanStaleTracks();
    }

    // if there are no files, we need to call complete() anyway
    if (maxQueueSize == 0) {
        complete();
        return;
    }

}

void CollectionScanner::stop() {
    qDebug() << "Scan stopped";
    working = false;
}

void CollectionScanner::complete() {
    qDebug() << "Scan complete";
    // Database::instance().getConnection().commit();
    Database::instance().setStatus(ScanComplete);
    Database::instance().setLastUpdate(QDateTime::currentDateTime().toTime_t());
    Database::instance().toDisk();

    // cleanup
    /*
    QList<Artist*> artists = loadedArtists.values();
    while (!artists.isEmpty())
        delete artists.takeFirst();
    */

    working = false;

    emit finished();
}

void CollectionScanner::setDirectory(QDir directory) {
    if (working) {
        emit error("A scanning task is already running");
        return;
    }
    this->rootDirectory = directory;
}

void CollectionScanner::scanDirectory(QDir directory) {
    // qDebug() << directory.absolutePath();
    directory.setFilter(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    QFileInfoList list = directory.entryInfoList();
    for (int i = 0; working && i < list.size(); ++i) {
        QFileInfo fileInfo = list.at(i);
        if (fileInfo.isDir()) {
            // this is a directory, recurse
            scanDirectory(QDir(fileInfo.absoluteFilePath()));
        } else {
            // this is a file, let's scan it
            processFile(fileInfo);
        }
    }
}

void CollectionScanner::processFile(QFileInfo fileInfo) {
    // qDebug() << "FILE:" << fileInfo.absoluteFilePath();

    // 1GB limit
    static const int MAX_FILE_SIZE = 1024 * 1024 * 1024;
    // skip big files
    if (fileInfo.size() > MAX_FILE_SIZE) {
        qDebug() << "Skipping file:" << fileInfo.absoluteFilePath();
        return;
    }

    // TODO check for .cue file

    if (incremental) {
        // incremental scan: check for modified track

        // path relative to the root of the collection
        QString path = fileInfo.absoluteFilePath();
        path.remove(this->rootDirectory.absolutePath() + "/");

        QDateTime lastModified = fileInfo.lastModified();
        // qDebug() << lastModified.toTime_t() << lastUpdate;
        if (lastModified.toTime_t() > lastUpdate && Track::isModified(path, lastModified)) {
            // Track has been modified
            fileQueue << fileInfo;
        }

    } else {
        // non-incremental scan, i.e. first scan:
        // process every file
        fileQueue << fileInfo;
    }

}

/*** Artist ***/

void CollectionScanner::giveThisFileAnArtist(FileInfo *file) {

    // qApp->processEvents();

    const QString artistTag = file->getTags()->artist;

    // try to normalize the artist name to a simpler form
    const QString artistHash = DataUtils::normalizeTag(DataUtils::cleanTag(artistTag));

    if (loadedArtists.contains(artistHash)) {
        // this artist was already encountered
        file->setArtist(loadedArtists.value(artistHash));
        // qDebug() << "artist loaded giveThisFileAnAlbum" << file->getFileInfo().baseName();
        giveThisFileAnAlbum(file);

    } else if (filesWaitingForArtists.contains(artistHash)) {
        // this artist is already being processed
        // so we need to add ourselves to the list of waiting files
        // qDebug() << "artist being processed" << artistHash << file->getFileInfo().baseName();
        QList<FileInfo *> files = filesWaitingForArtists.value(artistHash);
        files.append(file);
        filesWaitingForArtists.insert(artistHash, files);
        // qDebug() << "FILES WAITING 4 ARTISTS" <<  artistHash << files << files.count();

    } else {
        // this artist name was never encountered
        // start processing it
        // qDebug() << "new artist processArtsist" << file->getFileInfo().baseName();
        processArtist(file);
    }
}

void CollectionScanner::processArtist(FileInfo *file) {

    Artist *artist = new Artist();
    const QString artistTag = file->getTags()->artist;
    artist->setName(DataUtils::cleanTag(artistTag));
    artist->setProperty("originalHash", artist->getHash());

    // qDebug() << "Processing artist:" << artist->getName() << artist->getHash();

    if (filesWaitingForArtists.contains(artist->getHash())) {
        qDebug() << "ERROR Processing artist multiple times!" << artist->getName();
    }

    if (loadedArtists.contains(artist->getHash())) {
        qDebug() << "ERROR Artist already processed!" << artist->getName();
    }

    // add this file to filesWaitingForArtists
    // this also acts as a lock for other files
    // when the info is ready, all waiting files will be processed
    QList<FileInfo *> files;
    files.append(file);
    filesWaitingForArtists.insert(artist->getHash(), files);

    connect(artist, SIGNAL(gotInfo()), SLOT(gotArtistInfo()));
    artist->fetchInfo();

    qApp->processEvents();
}

void CollectionScanner::gotArtistInfo() {

    // get the Artist that sent the signal
    Artist *artist = static_cast<Artist *>(sender());
    if (!artist) {
        qDebug() << "Cannot get sender";
        return;
    }
    // qDebug() << "got info for" << artist->getName();

    int artistId = Artist::idForName(artist->getName());
    if (artistId < 0) {
        // qDebug() << "We have a new promising artist:" << artist->getName();
        artist->insert();
        // TODO last insert id
        artistId = Artist::idForName(artist->getName());
    }
    artist->setId(artistId);

    const QString hash = artist->property("originalHash").toString();
    QList<FileInfo *> files = filesWaitingForArtists.value(hash);
    filesWaitingForArtists.remove(hash);
    loadedArtists.insert(hash, artist);
    // if (hash != artist->getHash())
    loadedArtists.insert(artist->getHash(), artist);

    // continue the processing of blocked files
    // qDebug() << files.size() << "files were waiting for artist" << artist->getName();
    foreach (FileInfo *file, files) {
        file->setArtist(artist);
        // qDebug() << "ready for album" << file->getFileInfo().baseName();
        giveThisFileAnAlbum(file);
    }

}

/*** Album ***/

void CollectionScanner::giveThisFileAnAlbum(FileInfo *file) {

    const QString albumTag = DataUtils::cleanTag(file->getTags()->album);

    // try to normalize the album title to a simpler form
    const QString albumHash = DataUtils::normalizeTag(albumTag);

    if (albumTag.isEmpty()) {
        processTrack(file);

    } else if (loadedAlbums.contains(albumHash)) {
        // this album was already encountered
        // qDebug() << loadedAlbums.value(albumHash) << "is already loaded";
        file->setAlbum(loadedAlbums.value(albumHash));
        processTrack(file);

    } else if (filesWaitingForAlbums.contains(albumHash)) {
        // this album title is already being processed
        // so we need to add ourselves to the list of waiting files
        // qDebug() << "will wait for album" << albumHash;
        QList<FileInfo *> files = filesWaitingForAlbums.value(albumHash);
        files.append(file);
        filesWaitingForAlbums.insert(albumHash, files);

    } else {
        // this album title was never encountered
        // start processing it
        // qDebug() << "new album processAlbum" << albumHash << filesWaitingForAlbums.keys();
        processAlbum(file);
    }
}

void CollectionScanner::processAlbum(FileInfo *file) {

    Album *album = new Album();
    const QString albumTag = file->getTags()->album;
    album->setTitle(DataUtils::cleanTag(albumTag));
    album->setYear(file->getTags()->year);
    album->setProperty("originalHash", album->getHash());

    Artist *artist = file->getArtist();
    if (artist && artist->getId() > 0) album->setArtist(artist);
    else qDebug() << "Album" << album->getTitle() << "lacks an artist";
    // qDebug() << "Processing album:" << album->getTitle() << album->getHash();

    if (loadedAlbums.contains(album->getHash())) {
        qDebug() << "ERROR Album already processed!" << album->getTitle() << album->getHash();
        return;
    }
    if (filesWaitingForAlbums.contains(album->getHash())) {
        qDebug() << "ERROR Processing album multiple times!"
                << album->getTitle() << album->getHash() << file->getFileInfo().baseName();
        return;
    }

    // add this file to filesWaitingForAlbums
    // this also acts as a lock for other files
    // when the info is ready, all waiting files will be processed
    QList<FileInfo *> files;
    files.append(file);
    filesWaitingForAlbums.insert(album->getHash(), files);

    connect(album, SIGNAL(gotInfo()), SLOT(gotAlbumInfo()));
    album->fetchInfo();

    qApp->processEvents();
}

void CollectionScanner::gotAlbumInfo() {
    // get the Album that sent the signal
    Album *album = static_cast<Album *>(sender());
    if (!album) {
        qDebug() << "Cannot get sender";
        return;
    }

    const QString hash = album->property("originalHash").toString();
    // qDebug() << "got info for album" << album->getTitle() << hash << album->getHash();

    QList<FileInfo *> files = filesWaitingForAlbums.value(hash);
    filesWaitingForAlbums.remove(hash);
    loadedAlbums.insert(hash, album);
    // if (hash != album->getHash())
    loadedAlbums.insert(album->getHash(), album);

    int albumId = Album::idForName(album->getTitle());
    album->setId(albumId);
    if (albumId < 0) {
        qDebug() << "We have a new cool album:" << album->getTitle();
        album->insert();
        // TODO last insert id
        albumId = Album::idForName(album->getTitle());
    }
    album->setId(albumId);

    // continue the processing of blocked files
    // qDebug() << files.size() << "files were waiting for album" << album->getTitle();
    foreach (FileInfo *file, files) {
        file->setAlbum(album);
        processTrack(file);
    }

}

/*** Track ***/

void CollectionScanner::processTrack(FileInfo *file) {
    Track *track = new Track();
    QString titleTag = file->getTags()->title;
    // qDebug() << "we have a fresh track:" << titleTag;
    if (titleTag.isEmpty()) {
        titleTag = "[FILENAME] " + file->getFileInfo().baseName();
        // TODO clean filename:
        // strip constant part of the filenames
        // strip track number
    }

    Artist *artist = file->getArtist();
    if (artist && artist->getId() > 0) track->setArtist(artist);
    // else qDebug() << "track"<< track->getTitle() << "has no artist";

    Album *album = file->getAlbum();
    if (album && album->getId() > 0) track->setAlbum(album);
    // else qDebug() << "track"<< track->getTitle() << "has no album";
    if  (!album->getArtist()) {
        album->setArtist(artist);
    }

    track->setTitle(DataUtils::cleanTag(titleTag));

    // remove collection root path
    QString path = file->getFileInfo().absoluteFilePath();
    path.remove(this->rootDirectory.absolutePath() + "/");
    track->setPath(path);

    track->setNumber(file->getTags()->track);
    int year = album->getYear();
    if (year < 1) year = file->getTags()->year;
    track->setYear(year);

    // if (artist && artist->getId() > 0) {
    // artist = album->getArtist();
    track->setArtist(artist);
    // }

    // qDebug() << "Removing" << file->getFileInfo().baseName() << "from queue";
    if (!fileQueue.removeAll(file->getFileInfo())) {
        qDebug() << "Cannot remove file from queue";
    }

    connect(track, SIGNAL(gotInfo()), SLOT(gotTrackInfo()));
    track->fetchInfo();

    qApp->processEvents();
    // http://musicbrainz.org/ws/1/release/579278d5-75dc-4d2f-a5f3-6cc86f6c510e?type=xml&inc=tracks

}

void CollectionScanner::gotTrackInfo() {

    // get the Track that sent the signal
    Track *track = static_cast<Track *>(sender());
    if (!track) {
        qDebug() << "Cannot get sender";
        return;
    }
    // qDebug() << "got info for track" << track->getTitle();

    if (incremental && Track::exists(track->getPath())) {
        qDebug() << "Updating track:" << track->getTitle();
        track->update();
    } else {
        // qDebug() << "We have a new cool track:" << track->getTitle();
        track->insert();
    }

    /*
    qDebug() << "tracks:" << fileQueue.size()
            << "albums:" << filesWaitingForAlbums.size()
            << "artists:" << filesWaitingForArtists.size();
            */


    int percent = (maxQueueSize - fileQueue.size()) * 100 / maxQueueSize;
    // qDebug() << percent << "%";
    emit progress(percent);

    if (fileQueue.isEmpty()) {
        complete();
        exit();
    }

    /*
    else if (fileQueue.size() < 60) {
        foreach (FileInfo *file, fileQueue) {
            qDebug() << file->getFileInfo().filePath() << file->getTags()->title << file->getAlbum() << file->getArtist();
        }
    } */

}

void CollectionScanner::cleanStaleTracks() {
    QSettings settings;
    QString collectionRoot = settings.value("collectionRoot").toString()  + "/";
    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("select path from tracks");
    bool success = query.exec();
    if (!success) qDebug() << query.lastError().text();
    while (query.next()) {
        QString path = query.value(0).toString();
        if (!QFile::exists(collectionRoot + path)) {
            qDebug() << "Removing track" << path;
            Track::remove(path);
        }
    }
}