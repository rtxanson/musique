#include "track.h"
#include "album.h"
#include "artist.h"

#include "../database.h"
#include "../datautils.h"
#include <QtSql>

#include "../networkaccess.h"
#include <QtNetwork>
#include "../mbnetworkaccess.h"

Track::Track() {
    album = 0;
    artist = 0;
    number = 0;
    year = 0;
    length = 0;
    start = 0;
    end = 0;
}

static QHash<int, Track*> trackCache;

Track* Track::forId(int trackId) {

    if (trackCache.contains(trackId)) {
        // get from cache
        // qDebug() << "Track was cached" << trackId;
        return trackCache.value(trackId);
    }

    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("select path, title, start, end, duration, track, artist, album from tracks where id=?");
    query.bindValue(0, trackId);
    bool success = query.exec();
    if (!success) qDebug() << query.lastQuery() << query.lastError().text();
    if (query.next()) {
        Track* track = new Track();
        track->setId(trackId);
        track->setPath(query.value(0).toString());
        track->setTitle(query.value(1).toString());
        // TODO other fields
        track->setNumber(query.value(5).toInt());

        // relations
        int artistId = query.value(6).toInt();
        track->setArtist(Artist::forId(artistId));
        int albumId = query.value(7).toInt();
        track->setAlbum(Album::forId(albumId));

        // put into cache
        trackCache.insert(trackId, track);

        return track;
    }

    // id not found
    trackCache.insert(trackId, 0);
    return 0;
}

Track* Track::forPath(QString path) {
    Track *track = 0;
    int id = Track::idForPath(path);
    if (id != -1) track = Track::forId(id);
    return track;
}

int Track::idForPath(QString path) {
    int id = -1;
    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("select id from tracks where path=?");
    query.bindValue(0, path);
    bool success = query.exec();
    if (!success) qDebug() << query.lastError().text();
    if (query.next()) {
        id = query.value(0).toInt();
    }
    return id;
}

bool Track::exists(QString path) {
    // qDebug() << "Track::exists";
    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("select id from tracks where path=?");
    query.bindValue(0, path);
    bool success = query.exec();
    if (!success) qDebug() << query.lastError().text();
    return query.next();
}

bool Track::isModified(QString path, QDateTime lastModified) {
    // qDebug() << "Track::isModified";
    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("select id from tracks where path=? and tstamp>?");
    query.bindValue(0, path);
    query.bindValue(1, lastModified.toTime_t());
    qDebug() << query.lastQuery() << query.boundValues().values();
    bool success = query.exec();
    if (!success) qDebug() << query.lastError().text();
    // if we have no record then track has changed
    // (or it has never been put in the db)
    return !query.next();
}

void Track::insert() {
    // qDebug() << "Track::insert";
    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("insert into tracks (path, title, track, year, album, artist, tstamp) values (?,?,?,?,?,?,?)");
    query.bindValue(0, path);
    query.bindValue(1, title);
    query.bindValue(2, number);
    query.bindValue(3, year);
    int albumId = album ? album->getId() : 0;
    query.bindValue(4, albumId);
    int artistId = artist ? artist->getId() : 0;
    query.bindValue(5, artistId);
    query.bindValue(6, QDateTime::currentDateTime().toTime_t());
    bool success = query.exec();
    if (!success) qDebug() << query.lastError().text();

    // increment artist's track count
    if (artist && artist->getId()) {
        QSqlQuery query(db);
        query.prepare("update artists set trackCount=trackCount+1 where id=?");
        query.bindValue(0, artist->getId());
        bool success = query.exec();
        if (!success) qDebug() << query.lastError().text();
    }

    // increment album's track count
    if (album && album->getId()) {
        QSqlQuery query(db);
        query.prepare("update albums set trackCount=trackCount+1 where id=?");
        query.bindValue(0, album->getId());
        bool success = query.exec();
        if (!success) qDebug() << query.lastError().text();
    }
}

void Track::update() {
    // qDebug() << "Track::update";
    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("update tracks set title=?, track=?, year=?, album=?, artist=?, tstamp=? where path=?");

    query.bindValue(0, title);
    query.bindValue(1, number);
    query.bindValue(2, year);
    int albumId = album ? album->getId() : 0;
    query.bindValue(3, albumId);
    int artistId = artist ? artist->getId() : 0;
    query.bindValue(4, artistId);
    query.bindValue(5, QDateTime().toTime_t());
    query.bindValue(6, path);
    bool success = query.exec();
    if (!success) qDebug() << query.lastError().text();
}

void Track::remove(QString path) {
    // qDebug() << "Track::remove";
    QSqlDatabase db = Database::instance().getConnection();
    QSqlQuery query(db);
    query.prepare("delete from tracks where path=?");
    query.bindValue(0, path);
    bool success = query.exec();
    if (!success) qDebug() << query.lastError().text();
}

QString Track::getHash() {
    return Track::getHash(title);
}

QString Track::getHash(QString name) {
    // return DataUtils::calculateHash(DataUtils::normalizeTag(name));
    return DataUtils::normalizeTag(name);
}

void Track::fetchInfo() {
    emit gotInfo();
    // fetchMusicBrainzTrack();
}

// *** MusicBrainz ***

void Track::fetchMusicBrainzTrack() {

    QString s = "http://musicbrainz.org/ws/1/track/?type=xml&title=%1&limit=1";
    s = s.arg(title);
    if (artist) {
        s = s.append("&artist=%2").arg(artist->getName());
    };

    QUrl url(s);
    MBNetworkAccess *http = new MBNetworkAccess();
    QObject *reply = http->get(url);
    connect(reply, SIGNAL(data(QByteArray)), SLOT(parseMusicBrainzTrack(QByteArray)));
    connect(reply, SIGNAL(error(QNetworkReply*)), SIGNAL(gotInfo()));
}

void Track::parseMusicBrainzTrack(QByteArray bytes) {
    QString correctTitle = DataUtils::getXMLElementText(bytes, "title");
    qDebug() << title << "-> MusicBrainz ->" << correctTitle;
    if (!correctTitle.isEmpty()) {
        this->title = correctTitle;
    }

    emit gotInfo();
}

QString Track::getAbsolutePath() {
    QSettings settings;
    QString collectionRoot = settings.value("collectionRoot").toString();
    QString absolutePath = collectionRoot + "/" + path;
    return absolutePath;
}

QString Track::getLyrics() {
    return QString();
}

int Track::getTotalLength(QList<Track *>tracks) {
    int length = 0;
    foreach (Track* track, tracks) {
        length += track->getLength();
    }
    return length;
}