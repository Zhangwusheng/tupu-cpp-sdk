/******************************************************************************
 * TUPU Recognition API SDK
 * Copyright(c)2013-2016, TUPU Technology
 * http://www.tuputech.com
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <sstream>

#include "TImage.hpp"

using namespace std;

namespace TUPU
{


TImage::TImage()
    : m_buffer(NULL)
    , m_len(0)
{}

TImage::TImage(const TImage & img)
    : m_buffer(NULL)
    , m_len(0)
{
    m_tag = img.m_tag;
    if (img.m_buffer) {
        setBinary(img.m_buffer, img.m_len, img.m_filename);
    } else {
        m_url = img.m_url;
        m_path = img.m_path;
    }
}

TImage::~TImage()
{
    free(m_buffer);
}

TImage & TImage::operator=(const TImage & img)
{
    m_tag = img.m_tag;
    if (img.m_buffer) {
        setBinary(img.m_buffer, img.m_len, img.m_filename);
    } else {
        m_url = img.m_url;
        m_path = img.m_path;
    }

    return *this;
}


void TImage::setURL(const string & url)
{
    m_url = url;
    m_path.clear();
    free(m_buffer);
    m_len = 0;
    m_filename.clear();
}

void TImage::setPath(const string & filepath)
{
    m_path = filepath;
    m_url.clear();
    free(m_buffer);
    m_len = 0;
    m_filename.clear();
}

void TImage::setBinary(const void * buf, size_t buf_len, const string & filename)
{
    m_len = buf_len;
    m_filename = filename;
    m_buffer = malloc(buf_len);
    memcpy(m_buffer, buf, buf_len);

    m_url.clear();
    m_path.clear();
}



} //namespace TUPU