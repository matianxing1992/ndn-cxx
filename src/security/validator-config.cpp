/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil -*- */
/**
 * Copyright (C) 2013 Regents of the University of California.
 * See COPYING for copyright and distribution information.
 */

#include "validator-config.hpp"
#include "certificate-cache-ttl.hpp"
#include "../util/io.hpp"

#include <boost/filesystem.hpp>
#include <boost/property_tree/info_parser.hpp>
#include <boost/algorithm/string.hpp>

namespace ndn {

const shared_ptr<CertificateCache> ValidatorConfig::DEFAULT_CERTIFICATE_CACHE;

ValidatorConfig::ValidatorConfig(shared_ptr<Face> face,
                                 shared_ptr<CertificateCache> certificateCache,
                                 const int stepLimit)
  : Validator(face)
  , m_stepLimit(stepLimit)
  , m_certificateCache(certificateCache)
{
  if (!static_cast<bool>(face))
    throw Error("Face is not set!");

  if (!static_cast<bool>(m_certificateCache))
    m_certificateCache = make_shared<CertificateCacheTtl>(m_face->ioService());
}

void
ValidatorConfig::load(const std::string& filename)
{
  std::ifstream inputFile;
  inputFile.open(filename.c_str());
  if (!inputFile.good() || !inputFile.is_open())
    {
      std::string msg = "Failed to read configuration file: ";
      msg += filename;
      throw security::conf::Error(msg);
    }
  load(inputFile, filename);
  inputFile.close();
}

void
ValidatorConfig::load(const std::string& input, const std::string& filename)
{
  std::istringstream inputStream(input);
  load(inputStream, filename);
}


void
ValidatorConfig::load(std::istream& input, const std::string& filename)
{
  security::conf::ConfigSection tree;
  try
    {
      boost::property_tree::read_info(input, tree);
    }
  catch (const boost::property_tree::info_parser_error& error)
    {
      std::stringstream msg;
      msg << "Failed to parse configuration file";
      msg << " " << filename;
      msg << " " << error.message() << " line " << error.line();
      throw security::conf::Error(msg.str());
    }

  load(tree, filename);
}

void
ValidatorConfig::load(const security::conf::ConfigSection& configSection,
                      const std::string& filename)
{
  BOOST_ASSERT(!filename.empty());

  if (configSection.begin() == configSection.end())
    {
      std::string msg = "Error processing configuration file";
      msg += ": ";
      msg += filename;
      msg += " no data";
      throw security::conf::Error(msg);
    }

  for (security::conf::ConfigSection::const_iterator i = configSection.begin();
       i != configSection.end(); ++i)
    {
      const std::string& sectionName = i->first;
      const security::conf::ConfigSection& section = i->second;

      if (boost::iequals(sectionName, "rule"))
        {
          onConfigRule(section, filename);
        }
      else if (boost::iequals(sectionName, "trust-anchor"))
        {
          onConfigTrustAnchor(section, filename);
        }
      else
        {
          std::string msg = "Error processing configuration file";
          msg += " ";
          msg += filename;
          msg += " unrecognized section: " + sectionName;
          throw security::conf::Error(msg);
        }
    }
}

void
ValidatorConfig::onConfigRule(const security::conf::ConfigSection& configSection,
                              const std::string& filename)
{
  using namespace ndn::security::conf;

  ConfigSection::const_iterator propertyIt = configSection.begin();

  // Get rule.id
  if (propertyIt == configSection.end() || !boost::iequals(propertyIt->first, "id"))
    throw Error("Expect <rule.id>!");

  std::string ruleId = propertyIt->second.data();
  propertyIt++;

  // Get rule.for
  if (propertyIt == configSection.end() || !boost::iequals(propertyIt->first,"for"))
    throw Error("Expect <rule.for> in rule: " + ruleId + "!");

  std::string usage = propertyIt->second.data();
  propertyIt++;

  bool isForData;
  if (boost::iequals(usage, "data"))
    isForData = true;
  else if (boost::iequals(usage, "interest"))
    isForData = false;
  else
    throw Error("Unrecognized <rule.for>: " + usage
                + " in rule: " + ruleId);

  // Get rule.filter(s)
  std::vector<shared_ptr<Filter> > filters;
  for (; propertyIt != configSection.end(); propertyIt++)
    {
      if (!boost::iequals(propertyIt->first, "filter"))
        {
          if (boost::iequals(propertyIt->first, "checker"))
            break;
          throw Error("Expect <rule.filter> in rule: " + ruleId);
        }

      filters.push_back(FilterFactory::create(propertyIt->second));
      continue;
    }

  // Get rule.checker(s)
  std::vector<shared_ptr<Checker> > checkers;
  for (; propertyIt != configSection.end(); propertyIt++)
    {
      if (!boost::iequals(propertyIt->first, "checker"))
        throw Error("Expect <rule.checker> in rule: " + ruleId);

      checkers.push_back(CheckerFactory::create(propertyIt->second, filename));
      continue;
    }

  // Check other stuff
  if (propertyIt != configSection.end())
    throw Error("Expect the end of rule: " + ruleId);

  if (checkers.size() == 0)
    throw Error("No <rule.checker> is specified in rule: " + ruleId);

  if (isForData)
    {
      shared_ptr<DataRule> rule(new DataRule(ruleId));
      for (size_t i = 0; i < filters.size(); i++)
        rule->addFilter(filters[i]);
      for (size_t i = 0; i < checkers.size(); i++)
        rule->addChecker(checkers[i]);

      m_dataRules.push_back(rule);
    }
  else
    {
      shared_ptr<InterestRule> rule(new InterestRule(ruleId));
      for (size_t i = 0; i < filters.size(); i++)
        rule->addFilter(filters[i]);
      for (size_t i = 0; i < checkers.size(); i++)
        rule->addChecker(checkers[i]);

      m_interestRules.push_back(rule);
    }
}

void
ValidatorConfig::onConfigTrustAnchor(const security::conf::ConfigSection& configSection,
                                     const std::string& filename)
{
  using namespace ndn::security::conf;
  using namespace boost::filesystem;

  ConfigSection::const_iterator propertyIt = configSection.begin();

  // Get trust-anchor.type
  if (propertyIt == configSection.end() || !boost::iequals(propertyIt->first, "type"))
    throw Error("Expect <trust-anchor.type>!");

  std::string type = propertyIt->second.data();
  propertyIt++;

  if (boost::iequals(type, "file"))
    {
      // Get trust-anchor.file
      if (propertyIt == configSection.end() || !boost::iequals(propertyIt->first,"file-name"))
        throw Error("Expect <trust-anchor.file-name>!");

      std::string file = propertyIt->second.data();
      propertyIt++;

      // Check other stuff
      if (propertyIt != configSection.end())
        throw Error("Expect the end of trust-anchor!");

      path certfilePath = absolute(file, path(filename).parent_path());
      shared_ptr<IdentityCertificate> idCert =
        io::load<IdentityCertificate>(certfilePath.string());

      if (static_cast<bool>(idCert))
        {
          BOOST_ASSERT(idCert->getName().size() >= 1);
          m_anchors[idCert->getName().getPrefix(-1)] = idCert;
        }
      else
        throw Error("Cannot read certificate from file: " +
                    certfilePath.native());

      return;
    }
  else if (boost::iequals(type, "base64"))
    {
      // Get trust-anchor.base64-string
      if (propertyIt == configSection.end() || !boost::iequals(propertyIt->first, "base64-string"))
        throw Error("Expect <trust-anchor.base64-string>!");

      std::stringstream ss(propertyIt->second.data());
      propertyIt++;

      // Check other stuff
      if (propertyIt != configSection.end())
        throw Error("Expect the end of trust-anchor!");

      shared_ptr<IdentityCertificate> idCert = io::load<IdentityCertificate>(ss);

      if (static_cast<bool>(idCert))
        {
          BOOST_ASSERT(idCert->getName().size() >= 1);
          m_anchors[idCert->getName().getPrefix(-1)] = idCert;
        }
      else
        throw Error("Cannot decode certificate from base64-string");

      return;
    }
  else
    throw Error("Unsupported trust-anchor.type: " + type);
}

void
ValidatorConfig::checkPolicy(const Data& data,
                             int stepCount,
                             const OnDataValidated& onValidated,
                             const OnDataValidationFailed& onValidationFailed,
                             std::vector<shared_ptr<ValidationRequest> >& nextSteps)
{
  if (m_stepLimit == stepCount)
    return onValidationFailed(data.shared_from_this(),
                              "Maximum steps of validation reached");

  bool isMatched = false;
  int8_t checkResult = -1;

  for (DataRuleList::iterator it = m_dataRules.begin();
       it != m_dataRules.end(); it++)
    {
      if ((*it)->match(data))
        {
          isMatched = true;
          checkResult = (*it)->check(data, onValidated, onValidationFailed);
          break;
        }
    }

  if (!isMatched)
    return onValidationFailed(data.shared_from_this(), "No rule matched!");

  if (checkResult == 0)
    {
      const Signature& signature = data.getSignature();
      checkSignature(data, signature, stepCount,
                     onValidated, onValidationFailed, nextSteps);
    }
}

void
ValidatorConfig::checkPolicy(const Interest& interest,
                             int stepCount,
                             const OnInterestValidated& onValidated,
                             const OnInterestValidationFailed& onValidationFailed,
                             std::vector<shared_ptr<ValidationRequest> >& nextSteps)
{
  if (m_stepLimit == stepCount)
    return onValidationFailed(interest.shared_from_this(),
                              "Maximum steps of validation reached");

  bool isMatched = false;
  int8_t checkResult = -1;

  for (InterestRuleList::iterator it = m_interestRules.begin();
       it != m_interestRules.end(); it++)
    {
      if ((*it)->match(interest))
        {
          isMatched = true;
          checkResult = (*it)->check(interest, onValidated, onValidationFailed);
          break;
        }
    }

  if (!isMatched)
    return onValidationFailed(interest.shared_from_this(), "No rule matched!");

  if (checkResult == 0)
    {
      const Name& interestName = interest.getName();
      Name signedName = interestName.getPrefix(-2);
      Signature signature(interestName[-2].blockFromValue(),
                          interestName[-1].blockFromValue());

      checkSignature(interest, signature, stepCount,
                     onValidated, onValidationFailed, nextSteps);
    }
}


} // namespace ndn
