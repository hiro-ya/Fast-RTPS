// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "GovernanceParser.h"
#include <fastrtps/log/Log.h>

#include <cstring>
#include <cassert>

static const char* Root_str = "dds";
static const char* DomainAccessRules_str = "domain_access_rules";
static const char* DomainRule_str = "domain_rule";
static const char* Domains_str = "domains";
static const char* RtpsProtectionKind_str = "rtps_protection_kind";

static const char* ProtectionKindNone_str = "NONE";
static const char* ProtectionKindSign_str = "SIGN";
static const char* ProtectionKindEncrypt_str = "ENCRYPT";

using namespace eprosima::fastrtps::rtps::security;

bool GovernanceParser::parse_stream(const char* stream, size_t stream_length)
{
    assert(stream);

    bool returned_value = false;
    tinyxml2::XMLDocument document;

    if(tinyxml2::XMLError::XML_SUCCESS == document.Parse(stream, stream_length))
    {
        tinyxml2::XMLElement* root = document.RootElement();

        if(root != nullptr)
        {
            if(strcmp(root->Name(), Root_str) == 0)
            {
                returned_value = parse_domain_access_rules_node(root);
            }
            else
            {
                logError(XMLPARSER, "Malformed Governance root. Line " << root->GetLineNum());
            }
        }
        else
        {
            logError(XMLPARSER, "Not found root node in Governance XML.");
        }
    }
    else
    {
        logError(XMLPARSER, "Error loading Governance XML");
    }

    return returned_value;
}

bool GovernanceParser::parse_domain_access_rules_node(tinyxml2::XMLElement* root)
{
    assert(root);

    bool returned_value = false;
    tinyxml2::XMLElement* node = root->FirstChildElement();

    if(node != nullptr)
    {
        if(strcmp(node->Name(), DomainAccessRules_str) == 0)
        {
            if(parse_domain_access_rules(node))
            {
                if(node->NextSibling() == nullptr)
                {
                    returned_value = true;
                }
                else
                {
                    logError(XMLPARSER, "Only permitted one " << DomainAccessRules_str <<" tag. Line " <<
                            node->NextSibling()->GetLineNum());
                }
            }
        }
        else
        {
            logError(XMLPARSER, "Invalid tag. Expected " << DomainAccessRules_str << " tag. Line " << node->GetLineNum());
        }
    }
    else
    {
        logError(XMLPARSER, "Expected " << DomainAccessRules_str << " tag after root. Line " << root->GetLineNum() + 1);
    }

    return returned_value;
}

bool GovernanceParser::parse_domain_access_rules(tinyxml2::XMLElement* root)
{
    assert(root);

    bool returned_value = false;
    tinyxml2::XMLElement* node = root->FirstChildElement();

    if(node != nullptr)
    {
        returned_value = true;

        do
        {
            if(strcmp(node->Name(), DomainRule_str) == 0)
            {
                DomainRule domain_rule;

                if((returned_value = parse_domain_rule(node, domain_rule)) == true)
                {
                    access_rules_.rules.push_back(std::move(domain_rule));
                }
            }
            else
            {
                returned_value = false;
                logError(XMLPARSER, "Expected " << DomainRule_str << " tag. Line " << node->GetLineNum());
            }
        }
        while(returned_value && (node = node->NextSiblingElement()) != nullptr);
    }
    else
    {
        logError(XMLPARSER, "Minimum one " << DomainRule_str << " tag. Line " << root->GetLineNum() + 1);
    }

    return returned_value;
}

bool GovernanceParser::parse_domain_rule(tinyxml2::XMLElement* root, DomainRule& rule)
{
    assert(root);

    tinyxml2::XMLElement* node = root->FirstChildElement();
    tinyxml2::XMLElement* old_node = nullptr;

    if(node != nullptr)
    {
        if(strcmp(node->Name(), Domains_str) == 0)
        {
            if(!parse_domain_id_set(node, rule.domains))
            {
                return false;
            }
        }
        else
        {
            logError(XMLPARSER, "Expected " << Domains_str << " tag. Line " << node->GetLineNum());
            return false;
        }
    }
    else
    {
        logError(XMLPARSER, "Expected " << Domains_str << " tag. Line " << root->GetLineNum() + 1);
        return false;
    }

    old_node = node;
    node = node->NextSiblingElement();

    if(node != nullptr)
    {
        if(strcmp(node->Name(), RtpsProtectionKind_str) == 0)
        {
            const char* text = node->GetText();

            if(text != nullptr)
            {
                if(strcmp(text, ProtectionKindNone_str) == 0)
                {
                    rule.rtps_protection_kind = NONE;
                }
                else if(strcmp(text, ProtectionKindSign_str) == 0)
                {
                    rule.rtps_protection_kind = SIGN;
                }
                else if(strcmp(text, ProtectionKindEncrypt_str) == 0)
                {
                    rule.rtps_protection_kind = ENCRYPT;
                }
                else
                {
                    logError(XMLPARSER, "Invalid text in" << RtpsProtectionKind_str << " tag. Line " << node->GetLineNum());
                    return false;
                }
            }
            else
            {
                logError(XMLPARSER, "Expected text in" << RtpsProtectionKind_str << " tag. Line " << node->GetLineNum());
                return false;
            }
        }
        else
        {
            logError(XMLPARSER, "Expected " << RtpsProtectionKind_str << " tag. Line " << node->GetLineNum());
            return false;
        }
    }
    else
    {
        logError(XMLPARSER, "Expected " << RtpsProtectionKind_str << " tag. Line " << old_node->GetLineNum() + 1);
        return false;
    }

    node = node->NextSiblingElement();

    if(node != nullptr)
    {
        logError(XMLPARSER, "Not expected other tag. Line " << node->GetLineNum());
        return false;
    }

    return true;
}
