/**
 * Copyright (C) 2018 - 2021 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#ifndef __Section_h_
#define __Section_h_

// ----------------------- I N C L U D E S -----------------------------------

// #includes here - please keep these to a bare minimum!
#include "xclbin.h"

#include <string>
#include <fstream>
#include <map>
#include <functional>
#include <vector>

#include <boost/property_tree/ptree.hpp>

// ------------ F O R W A R D - D E C L A R A T I O N S ----------------------
// Forward declarations - use these instead whenever possible...

// ------------------- C L A S S :   S e c t i o n ---------------------------

class Section {
 public:
  enum FormatType{
    FT_UNDEFINED,
    FT_UNKNOWN,
    FT_RAW,
    FT_JSON,
    FT_HTML,
    FT_TXT
  };

 public:
  virtual ~Section();

 public:
  static void getKinds(std::vector< std::string > & kinds);
  static Section* createSectionObjectOfKind(enum axlf_section_kind _eKind, const std::string _sIndexName = "");
  static void translateSectionKindStrToKind(const std::string & sKind, enum axlf_section_kind & eKind);
  static bool getKindOfJSON(const std::string &_sJSONStr, enum axlf_section_kind &_eKind);
  static std::string getJSONOfKind(enum axlf_section_kind _eKind);
  static enum FormatType getFormatType(const std::string _sFormatType);
  static bool supportsSubSections(enum axlf_section_kind &_eKind);
  static bool supportsSectionIndex(enum axlf_section_kind &_eKind);

 public:
  virtual bool doesSupportAddFormatType(FormatType _eFormatType) const;
  virtual bool doesSupportDumpFormatType(FormatType _eFormatType) const;
  virtual bool supportsSubSection(const std::string &_sSubSectionName) const;
  virtual bool subSectionExists(const std::string &_sSubSectionName) const;

 public:
  enum axlf_section_kind getSectionKind() const;
  const std::string& getSectionKindAsString() const;
  std::string getName() const;
  unsigned int getSize() const;
  const std::string & getSectionIndexName() const;

 public:
  // Xclbin Binary helper methods - child classes can override them if they choose
  virtual void readXclBinBinary(std::fstream& _istream, const struct axlf_section_header& _sectionHeader);
  virtual void readXclBinBinary(std::fstream& _istream, const boost::property_tree::ptree& _ptSection);
  void readXclBinBinary(std::fstream& _istream, enum FormatType _eFormatType);
  void readJSONSectionImage(const boost::property_tree::ptree& _ptSection);
  void readPayload(std::fstream& _istream, enum FormatType _eFormatType);
  void printHeader(std::ostream &_ostream) const;
  bool getSubPayload(std::ostringstream &_buf, const std::string _sSubSection, enum Section::FormatType _eFormatType) const;
  void readSubPayload(std::istream& _istream, const std::string & _sSubSection, enum Section::FormatType _eFormatType);
  virtual void initXclBinSectionHeader(axlf_section_header& _sectionHeader);
  virtual void writeXclBinSectionBuffer(std::ostream& _ostream) const;
  virtual void appendToSectionMetadata(const boost::property_tree::ptree& _ptAppendData, boost::property_tree::ptree& _ptToAppendTo);

  void dumpContents(std::ostream& _ostream, enum FormatType _eFormatType) const;
  void dumpSubSection(std::fstream& _ostream, std::string _sSubSection, enum FormatType _eFormatType) const;

  void getPayload(boost::property_tree::ptree& _pt) const;
  void purgeBuffers();
  void setName(const std::string &_sSectionName);
  void setPathAndName(const std::string& _pathAndName);
  const std::string &getPathAndName() const;

 protected:
  // Child class option to create an JSON metadata
  virtual void marshalToJSON(char* _pDataSection, unsigned int _sectionSize, boost::property_tree::ptree& _ptree) const;
  virtual void marshalFromJSON(const boost::property_tree::ptree& _ptSection, std::ostringstream& _buf) const;
  virtual void getSubPayload(char* _pDataSection, unsigned int _sectionSize, std::ostringstream &_buf, const std::string &_sSubSection, enum Section::FormatType _eFormatType) const;
  virtual void readSubPayload(const char *_pOrigDataSection, unsigned int _origSectionSize,  std::istream &_istream, const std::string &_sSubSection, enum Section::FormatType _eFormatType, std::ostringstream &_buffer) const;
  virtual void writeSubPayload(const std::string & _sSubSectionName, FormatType _eFormatType, std::fstream&  _oStream) const;

 protected:
  Section();

 protected:
  typedef std::function<Section*()> Section_factory;
  static void registerSectionCtor(enum axlf_section_kind _eKind, const std::string& _sKindStr, const std::string& _sHeaderJSONName, bool _bSupportsSubSections, bool _bSupportsIndexing, Section_factory _Section_factory);

 protected:
  enum axlf_section_kind m_eKind;
  std::string m_sKindName;
  std::string m_sIndexName;

  char* m_pBuffer;
  unsigned int m_bufferSize;
  std::string m_name;

  std::string m_pathAndName;

 private:
  static std::map<enum axlf_section_kind, std::string> m_mapIdToName;
  static std::map<std::string, enum axlf_section_kind> m_mapNameToId;
  static std::map<enum axlf_section_kind, Section_factory> m_mapIdToCtor;
  static std::map<std::string, enum axlf_section_kind> m_mapJSONNameToKind;
  static std::map<enum axlf_section_kind, std::string> m_mapKindToJSONName;
  static std::map<enum axlf_section_kind, bool> m_mapIdToSubSectionSupport;
  static std::map<enum axlf_section_kind, bool> m_mapIdToSectionIndexSupport;

 private:
  Section(const Section& obj) = delete;
  Section& operator=(const Section& obj) = delete;
};

#endif
