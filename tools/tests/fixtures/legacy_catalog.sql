-- MySQL dump 10.13  Distrib 5.7.33, for Linux (x86_64)
--
-- Host: localhost    Database: pychrondvc
-- ------------------------------------------------------
-- Server version	5.7.33
--
-- Hand-written in mysqldump's shape from the legacy ORM (pychron/dvc/dvc_orm.py;
-- see tests/dvc/fixtures/README.md, section 7). It is the source of
-- tests/dvc/fixtures/catalog: after editing it run
--   python3 tools/legacy_dump_to_jsonl.py tools/tests/fixtures/legacy_catalog.sql tests/dvc/fixtures/catalog
-- The comments say which rows the catalog adapter must refuse, or import
-- without a link, and why.

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET NAMES utf8 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;

DROP TABLE IF EXISTS `PrincipalInvestigatorTbl`;
CREATE TABLE `PrincipalInvestigatorTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `affiliation` varchar(140) DEFAULT NULL,
  `email` varchar(140) DEFAULT NULL,
  `last_name` varchar(140) DEFAULT NULL,
  `first_initial` varchar(10) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=utf8;

LOCK TABLES `PrincipalInvestigatorTbl` WRITE;
/*!40000 ALTER TABLE `PrincipalInvestigatorTbl` DISABLE KEYS */;
INSERT INTO `PrincipalInvestigatorTbl` VALUES (1,'NMT','ross@nmt.edu','Ross','J'),(2,NULL,NULL,'Heizler',NULL),(3,'NMBG',NULL,'McIntosh','W');
/*!40000 ALTER TABLE `PrincipalInvestigatorTbl` ENABLE KEYS */;
UNLOCK TABLES;

DROP TABLE IF EXISTS `ProjectTbl`;
CREATE TABLE `ProjectTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(80) DEFAULT NULL,
  `principal_investigatorID` int(11) DEFAULT NULL,
  `checkin_date` date DEFAULT NULL,
  `comment` text,
  `lab_contact` varchar(80) DEFAULT NULL,
  `institution` varchar(80) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `principal_investigatorID` (`principal_investigatorID`),
  CONSTRAINT `projecttbl_ibfk_1` FOREIGN KEY (`principal_investigatorID`) REFERENCES `PrincipalInvestigatorTbl` (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=5 DEFAULT CHARSET=utf8;

-- 3: imported without its principal investigator, who is not in the dump.
LOCK TABLES `ProjectTbl` WRITE;
INSERT INTO `ProjectTbl` VALUES (1,'Henry Hill',1,'2016-02-29','two crates; \'handle\' with care','mheizler','NMT'),(2,'REFERENCES',NULL,NULL,NULL,NULL,NULL),(3,'Orphan',99,NULL,NULL,NULL,NULL),(4,'J-Curve',2,'0000-00-00',NULL,NULL,NULL);
UNLOCK TABLES;

DROP TABLE IF EXISTS `MaterialTbl`;
CREATE TABLE `MaterialTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(80) DEFAULT NULL,
  `grainsize` varchar(80) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=utf8;

-- 3 repeats 1: one material.
LOCK TABLES `MaterialTbl` WRITE;
INSERT INTO `MaterialTbl` VALUES (1,'Sanidine',NULL),(2,'Groundmass','250-500'),(3,'Sanidine',NULL);
UNLOCK TABLES;

DROP TABLE IF EXISTS `SampleTbl`;
CREATE TABLE `SampleTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(80) DEFAULT NULL,
  `materialID` int(11) DEFAULT NULL,
  `projectID` int(11) DEFAULT NULL,
  `note` varchar(140) DEFAULT NULL,
  `igsn` varchar(140) DEFAULT NULL,
  `lat` float DEFAULT NULL,
  `lon` float DEFAULT NULL,
  `storage_location` varchar(140) DEFAULT NULL,
  `lithology` varchar(140) DEFAULT NULL,
  `unit` varchar(80) DEFAULT NULL,
  `lithology_class` varchar(140) DEFAULT NULL,
  `lithology_type` varchar(140) DEFAULT NULL,
  `lithology_group` varchar(140) DEFAULT NULL,
  `location` varchar(140) DEFAULT NULL,
  `approximate_age` float DEFAULT NULL,
  `elevation` float DEFAULT NULL,
  `create_date` datetime DEFAULT NULL,
  `update_date` datetime DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `materialID` (`materialID`),
  KEY `projectID` (`projectID`)
) ENGINE=InnoDB AUTO_INCREMENT=6 DEFAULT CHARSET=utf8;

-- 3: refused, no such project. 5: refused, it is sample 1 again with another
-- note.
LOCK TABLES `SampleTbl` WRITE;
INSERT INTO `SampleTbl` VALUES (1,'HH-1',1,1,'collected at the base, north side','IGSN001',34.0722,-106.905,'shelf 3','ignimbrite','Tuff of Henry Hill','volcanic','pyroclastic','Mogollon','Socorro, NM',28.2,1890.5,'2016-03-01 09:30:00','2016-11-06 01:30:00'),(2,'FC-2',3,2,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL),(3,'Lost',1,42,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL),(4,'Orphan-1',1,3,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL),(5,'HH-1',1,1,'another note',NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
UNLOCK TABLES;

DROP TABLE IF EXISTS `IrradiationTbl`;
CREATE TABLE `IrradiationTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(80) DEFAULT NULL,
  `create_date` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=3 DEFAULT CHARSET=utf8;

LOCK TABLES `IrradiationTbl` WRITE;
INSERT INTO `IrradiationTbl` VALUES (1,'NM-300','2018-01-15 17:00:00'),(2,'NM-301','0000-00-00 00:00:00');
UNLOCK TABLES;

DROP TABLE IF EXISTS `LevelTbl`;
CREATE TABLE `LevelTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(80) DEFAULT NULL,
  `irradiationID` int(11) DEFAULT NULL,
  `holder` varchar(45) DEFAULT NULL,
  `z` float DEFAULT NULL,
  `note` text,
  PRIMARY KEY (`id`),
  KEY `irradiationID` (`irradiationID`)
) ENGINE=InnoDB AUTO_INCREMENT=5 DEFAULT CHARSET=utf8;

-- 4: refused, no such irradiation.
LOCK TABLES `LevelTbl` WRITE;
INSERT INTO `LevelTbl` VALUES (1,'A',1,'24Spokes',0.5,'bottom of the can'),(2,'B',1,NULL,NULL,NULL),(3,'A',2,'24Spokes',NULL,NULL),(4,'Z',7,NULL,NULL,NULL);
UNLOCK TABLES;

DROP TABLE IF EXISTS `IrradiationPositionTbl`;
CREATE TABLE `IrradiationPositionTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `identifier` varchar(80) DEFAULT NULL,
  `sampleID` int(11) DEFAULT NULL,
  `levelID` int(11) DEFAULT NULL,
  `position` int(11) DEFAULT NULL,
  `note` text,
  `weight` float DEFAULT NULL,
  `j` float DEFAULT NULL,
  `j_err` float DEFAULT NULL,
  `packet` varchar(40) DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `identifier` (`identifier`),
  KEY `sampleID` (`sampleID`),
  KEY `levelID` (`levelID`)
) ENGINE=InnoDB AUTO_INCREMENT=8 DEFAULT CHARSET=utf8;

-- 3: an empty hole. 5: imported without its sample, which was refused.
-- 6: refused, 66573 already sits at NM-300 A 1. 7: refused, no position.
LOCK TABLES `IrradiationPositionTbl` WRITE;
INSERT INTO `IrradiationPositionTbl` VALUES (1,'66573',1,1,1,'chipped',12.5,0.00125,2.1e-06,'p1'),(2,'66574',2,1,2,NULL,NULL,NULL,NULL,NULL),(3,NULL,NULL,1,3,NULL,NULL,NULL,NULL,NULL),(4,'66600',1,2,1,NULL,NULL,NULL,NULL,NULL),(5,'66601',3,2,2,NULL,NULL,NULL,NULL,NULL),(6,'66573',1,3,1,NULL,NULL,NULL,NULL,NULL),(7,'66700',NULL,2,NULL,NULL,NULL,NULL,NULL,NULL);
UNLOCK TABLES;

DROP TABLE IF EXISTS `UserTbl`;
CREATE TABLE `UserTbl` (
  `name` varchar(45) NOT NULL,
  `affiliation` varchar(80) DEFAULT NULL,
  `category` varchar(80) DEFAULT NULL,
  `email` varchar(80) DEFAULT NULL,
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

LOCK TABLES `UserTbl` WRITE;
INSERT INTO `UserTbl` VALUES ('jross',NULL,NULL,NULL),('mheizler','NMT','staff','m@nmt.edu');
UNLOCK TABLES;

DROP TABLE IF EXISTS `MassSpectrometerTbl`;
CREATE TABLE `MassSpectrometerTbl` (
  `name` varchar(45) NOT NULL,
  `kind` varchar(45) DEFAULT NULL,
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

LOCK TABLES `MassSpectrometerTbl` WRITE;
INSERT INTO `MassSpectrometerTbl` VALUES ('Felix','Helix SFT'),('Jan','Argus VI');
UNLOCK TABLES;

DROP TABLE IF EXISTS `ExtractDeviceTbl`;
CREATE TABLE `ExtractDeviceTbl` (
  `name` varchar(45) NOT NULL,
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

LOCK TABLES `ExtractDeviceTbl` WRITE;
INSERT INTO `ExtractDeviceTbl` VALUES ('Fusions CO2'),('Fusions Diode');
UNLOCK TABLES;

DROP TABLE IF EXISTS `LoadTbl`;
CREATE TABLE `LoadTbl` (
  `name` varchar(45) NOT NULL,
  `create_date` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `archived` tinyint(1) DEFAULT NULL,
  `username` varchar(140) DEFAULT NULL,
  `holderName` varchar(45) DEFAULT NULL,
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- L-103: imported without its user, who is not in the dump. L-104: its user
-- is 'jross' as MySQL compares names (case and trailing spaces ignored).
LOCK TABLES `LoadTbl` WRITE;
INSERT INTO `LoadTbl` VALUES ('L-101','2018-03-01 18:00:00',0,'mheizler','221-hole'),('L-102','2018-04-01 18:00:00',1,NULL,NULL),('L-103','2018-05-01 18:00:00',0,'nobody',NULL),('L-104','2018-06-01 18:00:00',0,'JRoss ',NULL);
UNLOCK TABLES;

DROP TABLE IF EXISTS `LoadPositionTbl`;
CREATE TABLE `LoadPositionTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `identifier` varchar(80) DEFAULT NULL,
  `position` int(11) DEFAULT NULL,
  `loadName` varchar(45) DEFAULT NULL,
  `weight` float DEFAULT NULL,
  `note` text,
  `nxtals` int(11) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=7 DEFAULT CHARSET=utf8;

-- 3: refused, no such identifier. 4: refused, no such load. 6: its load is
-- 'L-101' as MySQL compares names.
LOCK TABLES `LoadPositionTbl` WRITE;
INSERT INTO `LoadPositionTbl` VALUES (1,'66573',1,'L-101',1.5,'big',2),(2,'66574',2,'L-101',NULL,NULL,NULL),(3,'99999',3,'L-101',NULL,NULL,NULL),(4,'66573',1,'L-999',NULL,NULL,NULL),(5,'66601',1,'L-102',NULL,NULL,NULL),(6,'66600',4,'l-101 ',NULL,NULL,NULL);
UNLOCK TABLES;

DROP TABLE IF EXISTS `RepositoryTbl`;
CREATE TABLE `RepositoryTbl` (
  `name` varchar(80) NOT NULL,
  `principal_investigatorID` int(11) DEFAULT NULL,
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

LOCK TABLES `RepositoryTbl` WRITE;
INSERT INTO `RepositoryTbl` VALUES ('Henry_Hill',1);
UNLOCK TABLES;

DROP TABLE IF EXISTS `AnalysisTbl`;
CREATE TABLE `AnalysisTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `experiment_type` varchar(32) DEFAULT NULL,
  `timestamp` datetime DEFAULT NULL,
  `uuid` varchar(40) DEFAULT NULL,
  `analysis_type` varchar(45) DEFAULT NULL,
  `aliquot` int(11) DEFAULT NULL,
  `increment` int(11) DEFAULT NULL,
  `irradiation_positionID` int(11) DEFAULT NULL,
  `mass_spectrometer` varchar(45) DEFAULT NULL,
  `extract_device` varchar(45) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=5 DEFAULT CHARSET=utf8;

-- The uuid is stored without dashes (1, 3), with them (2) or in upper case (4).
LOCK TABLES `AnalysisTbl` WRITE;
INSERT INTO `AnalysisTbl` VALUES (1,'Ar/Ar','2018-03-02 10:00:00','0f8fad5bd9cb469fa16570867728950e','unknown',1,NULL,1,'Jan','Fusions CO2'),(2,'Ar/Ar','2018-03-02 11:00:00','7c9e6679-7425-40de-944b-e07fc1f90ae7','unknown',2,0,1,'Jan','Fusions CO2'),(3,'Ar/Ar','2018-03-02 12:00:00','11111111222243338444555555555555','unknown',3,NULL,1,'Jan','Fusions CO2'),(4,'Ar/Ar','2018-03-02 13:00:00','AAAAAAAABBBB4CCC8DDDEEEEEEEEEEEE','unknown',4,NULL,1,'Jan','Fusions CO2');
UNLOCK TABLES;

DROP TABLE IF EXISTS `AnalysisChangeTbl`;
CREATE TABLE `AnalysisChangeTbl` (
  `idanalysischangeTbl` int(11) NOT NULL AUTO_INCREMENT,
  `tag` varchar(40) DEFAULT NULL,
  `timestamp` timestamp NULL DEFAULT NULL,
  `user` varchar(40) DEFAULT NULL,
  `analysisID` int(11) DEFAULT NULL,
  PRIMARY KEY (`idanalysischangeTbl`)
) ENGINE=InnoDB AUTO_INCREMENT=6 DEFAULT CHARSET=utf8;

-- Analysis 1 has two rows (the later one counts), 3 has none, 4 has no tag;
-- row 5 names an analysis that is not in the dump.
LOCK TABLES `AnalysisChangeTbl` WRITE;
INSERT INTO `AnalysisChangeTbl` VALUES (1,'ok','2018-03-03 00:00:00','jross',1),(2,'invalid','2018-03-04 00:00:00','jross',2),(3,'omit','2018-03-05 00:00:00','jross',1),(4,NULL,'2018-03-05 00:00:00','jross',4),(5,'skip','2018-03-05 00:00:00','jross',77);
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;

-- Dump completed on 2021-06-07 12:00:00
