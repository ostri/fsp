#pragma once
#include <string>

struct xml_segment
{
  size_t      id;
  int         xpath_index;
  std::string raw_content;
};