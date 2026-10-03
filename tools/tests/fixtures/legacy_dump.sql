-- MySQL dump 10.13  Distrib 5.7.33, for Linux (x86_64)
--
-- Host: localhost    Database: pychrondvc
-- ------------------------------------------------------
-- Server version	5.7.33

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET NAMES utf8 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;

--
-- Table structure for table `MaterialTbl`
--

DROP TABLE IF EXISTS `MaterialTbl`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `MaterialTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(80) DEFAULT NULL,
  `grainsize` varchar(80) DEFAULT NULL COMMENT 'sieve fraction, e.g. 250-500 (um); none: NULL',
  PRIMARY KEY (`id`),
  KEY `name_ix` (`name`)
) ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=latin1;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `MaterialTbl`
--

LOCK TABLES `MaterialTbl` WRITE;
/*!40000 ALTER TABLE `MaterialTbl` DISABLE KEYS */;
INSERT INTO `MaterialTbl` VALUES (1,'Sanidine',NULL),(2,'Groundmass, \'fine\' (sieved)','250-500'),(3,'It''s; a "test"\\path\n2nd line\ttab',''),(4,'Biotite',NULL);
/*!40000 ALTER TABLE `MaterialTbl` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `SampleTbl`
--

DROP TABLE IF EXISTS `SampleTbl`;
CREATE TABLE `SampleTbl` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(80) DEFAULT NULL,
  `materialID` int(11) DEFAULT NULL,
  `lat` float DEFAULT NULL,
  `lon` double DEFAULT NULL,
  `note` varchar(140) DEFAULT 'a,b) (c',
  `create_date` datetime DEFAULT NULL,
  `kind` enum('rock','mineral (separate)') DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `name` (`name`),
  KEY `materialID` (`materialID`),
  CONSTRAINT `sampletbl_ibfk_1` FOREIGN KEY (`materialID`) REFERENCES `MaterialTbl` (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

LOCK TABLES `SampleTbl` WRITE;
INSERT INTO `SampleTbl` VALUES (1,'FC-2',1,37.75,-106.93,'Fish Canyon; (sanidine), \'neutron\' fluence monitor','2015-03-04 10:11:12','mineral (separate)');
INSERT INTO `SampleTbl` VALUES (2,'AC-1',NULL,-1.5e-3,1E2,NULL,NULL,NULL),(3,'x',4,NULL,-0.25,'',NULL,'rock');
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

-- Dump completed on 2021-06-07 12:00:00
