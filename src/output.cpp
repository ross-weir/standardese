// Copyright (C) 2016 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include <standardese/output.hpp>

#include <stack>
#include <spdlog/logger.h>

#include <standardese/comment.hpp>
#include <standardese/generator.hpp>
#include <standardese/index.hpp>
#include <standardese/linker.hpp>
#include <standardese/md_inlines.hpp>

using namespace standardese;

namespace
{
    using standardese::index;

    const char link_prefix[] = "standardese://";

    template <typename Func>
    void for_each_entity_reference(md_document& doc, Func f)
    {
        std::stack<std::pair<md_container*, const doc_entity*>> stack;
        stack.emplace(&doc, nullptr);
        while (!stack.empty())
        {
            auto cur = stack.top();
            stack.pop();

            for (auto& child : *cur.first)
            {
                if (child.get_entity_type() == md_entity::link_t)
                {
                    auto& link = static_cast<md_link&>(child);
                    if (*link.get_destination() == '\0')
                        // empty link
                        f(cur.second, link);
                    else if (std::strncmp(link.get_destination(), link_prefix,
                                          sizeof(link_prefix) - 1)
                             == 0)
                        // standardese link
                        f(cur.second, link);
                }
                else if (child.get_entity_type() == md_entity::comment_t)
                {
                    auto& comment = static_cast<md_comment&>(child);
                    stack.emplace(&comment, comment.has_entity() ? &comment.get_entity() : nullptr);
                }
                else if (is_container(child.get_entity_type()))
                {
                    auto& container = static_cast<md_container&>(child);
                    stack.emplace(&container, cur.second);
                }
            }
        }
    }

    std::string get_entity_name(const md_link& link)
    {
        if (*link.get_destination())
        {
            assert(std::strncmp(link.get_destination(), link_prefix, sizeof(link_prefix) - 1) == 0);
            std::string result = link.get_destination() + sizeof(link_prefix) - 1;
            result.pop_back();
            return result;
        }
        else if (*link.get_title())
            return link.get_title();
        else if (link.begin()->get_entity_type() != md_entity::text_t)
            // must be a text
            return "";
        auto& text = static_cast<const md_text&>(*link.begin());
        return text.get_string();
    }

    void resolve_urls(const std::shared_ptr<spdlog::logger>& logger, const index& i,
                      md_document& document, const char* extension)
    {
        for_each_entity_reference(document, [&](const doc_entity* context, md_link& link) {
            auto str = get_entity_name(link);
            if (str.empty())
                return;

            auto destination = i.get_linker().get_url(i, context, str, extension);
            if (destination.empty())
                logger->warn("unable to resolve link to an entity named '{}'", str);
            else
                link.set_destination(destination.c_str());
        });
    }
}

void standardese::normalize_urls(const index& idx, md_document& document)
{
    for_each_entity_reference(document, [&](const doc_entity* context, md_link& link) {
        auto str = get_entity_name(link);
        if (str.empty())
            return;

        auto entity = context ? idx.try_name_lookup(*context, str) : idx.try_lookup(str);
        if (entity)
            link.set_destination(
                (std::string("standardese://") + entity->get_unique_name().c_str() + '/').c_str());
    });
}

raw_document::raw_document(path fname, std::string text)
: file_name(std::move(fname)), text(std::move(text))
{
    auto idx = file_name.rfind('.');
    if (idx != path::npos)
    {
        file_extension = file_name.substr(idx + 1);
        file_name.erase(idx);
    }
}

void output::render(const std::shared_ptr<spdlog::logger>& logger, const md_document& doc,
                    const char* output_extension)
{
    if (!output_extension)
        output_extension = format_->extension();

    auto document = md_ptr<md_document>(static_cast<md_document*>(doc.clone().release()));
    resolve_urls(logger, *index_, *document, output_extension);

    file_output output(prefix_ + document->get_output_name() + '.' + output_extension);
    format_->render(output, *document);
}

void output::render_template(const std::shared_ptr<spdlog::logger>& logger,
                             const template_file& templ, const documentation& doc,
                             const char* output_extension)
{
    if (!output_extension)
        output_extension = format_->extension();

    auto document           = process_template(*parser_, *index_, templ, format_, &doc);
    document.file_name      = doc.document->get_output_name();
    document.file_extension = output_extension;

    render_raw(logger, document);
}

void output::render_raw(const std::shared_ptr<spdlog::logger>& logger, const raw_document& document)
{
    auto extension =
        document.file_extension.empty() ? format_->extension() : document.file_extension;
    file_output output(prefix_ + document.file_name + '.' + extension);

    auto last_match = document.text.c_str();
    // while we find standardese protocol URLs starting at last_match
    while (auto match = std::strstr(last_match, link_prefix))
    {
        // write from last_match to match
        output.write_str(last_match, match - last_match);

        // write correct URL
        auto        entity_name = match + sizeof(link_prefix) - 1;
        const char* end         = std::strchr(entity_name, '/');
        if (end == nullptr)
            end = &document.text.back() + 1;

        std::string name(entity_name, end - entity_name);
        auto url = index_->get_linker().get_url(*index_, nullptr, name, format_->extension());
        if (url.empty())
        {
            logger->warn("unable to resolve link to an entity named '{}'", name);
            output.write_str(match, entity_name - match);
            last_match = entity_name;
        }
        else
        {
            output.write_str(url.c_str(), url.size());
            last_match = entity_name + name.size() + 1;
        }
    }
    // write remainder of file
    output.write_str(last_match, &document.text.back() + 1 - last_match);
}
