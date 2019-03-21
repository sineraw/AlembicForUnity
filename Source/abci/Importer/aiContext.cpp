#include "pch.h"
#include "aiInternal.h"
#include "aiContext.h"
#include "aiObject.h"
#include "aiAsync.h"
#include <istream>

static std::wstring L(const std::string& s)
{
    return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(s);
}

static std::string S(const std::wstring& w)
{
    return std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(w);
}

static std::string NormalizePath(const char *in_path)
{
    std::wstring path;

    if (in_path != nullptr) {
        path = L(in_path);

#ifdef _WIN32
        size_t n = path.length();
        for (size_t i = 0; i < n; ++i) {
            auto c = path[i];
            if (c == L'\\') {
                path[i] = L'/';
            }
            else if (c >= L'A' && c <= L'Z') {
                path[i] = L'a' + (c - L'A');
            }
        }
#endif
    }

    return S(path);
}

#ifdef WIN32

class lockFreeIStream : public std::ifstream
{
private:
	FILE *_file = nullptr;
	FILE *Init(const wchar_t *name)
	{
		_file = _wfsopen(name, L"rb", _SH_DENYNO);
		return _file;
	}
public: 
	lockFreeIStream(const wchar_t  *name) : std::ifstream(Init(name))
	{
	}
	~lockFreeIStream()
	{
		if (_file != nullptr)
		{
			fclose(_file);
		}
	}


};
#endif

aiContextManager aiContextManager::s_instance;

aiContext* aiContextManager::getContext(int uid)
{
    auto it = s_instance.m_contexts.find(uid);
if (it != s_instance.m_contexts.end()) {
        DebugLog("Using already created context for gameObject with ID %d", uid);
        return it->second.get();
    }

    auto ctx = new aiContext(uid);
    s_instance.m_contexts[uid].reset(ctx);
    DebugLog("Register context for gameObject with ID %d", uid);
    return ctx;
}

void aiContextManager::destroyContext(int uid)
{
    auto it = s_instance.m_contexts.find(uid);
    if (it != s_instance.m_contexts.end()) {
        DebugLog("Unregister context for gameObject with ID %d", uid);
        s_instance.m_contexts.erase(it);
    }
}

void aiContextManager::destroyContextsWithPath(const char* asset_path)
{
    auto path = NormalizePath(asset_path);
    for (auto it = s_instance.m_contexts.begin(); it != s_instance.m_contexts.end();) {
        if (it->second->getPath() == path) {
            DebugLog("Unregister context for gameObject with ID %s", it->second->getPath().c_str());
            s_instance.m_contexts.erase(it++);
        }
        else {
            ++it;
        }
    }
}

aiContextManager::~aiContextManager()
{
    if (m_contexts.size()) {
        DebugWarning("%lu remaining context(s) registered", m_contexts.size());
    }
    m_contexts.clear();
}


aiContext::aiContext(int uid)
    : m_uid(uid)
{
}

aiContext::~aiContext()
{
    reset();
}

Abc::IArchive aiContext::getArchive() const
{
    return m_archive;
}

const std::string& aiContext::getPath() const
{
    return m_path;
}


int aiContext::getTimeSamplingCount() const
{
    return (int)m_timesamplings.size();
}

aiTimeSampling * aiContext::getTimeSampling(int i)
{
    return m_timesamplings[i].get();
}

void aiContext::getTimeRange(double& begin, double& end) const
{
    begin = end = 0.0;
    for (size_t i = 1; i < m_timesamplings.size(); ++i) {
        double tmp_begin, tmp_end;
        m_timesamplings[i]->getTimeRange(tmp_begin, tmp_end);

        if (i == 1) {
            begin = tmp_begin;
            end = tmp_end;
        }
        else {
            begin = std::min(begin, tmp_begin);
            end = std::max(end, tmp_end);
        }
    }
}

int aiContext::getTimeSamplingCount()
{
    return (int)m_timesamplings.size();
}

int aiContext::getTimeSamplingIndex(Abc::TimeSamplingPtr ts)
{
    int n = m_archive.getNumTimeSamplings();
    for (int i = 0; i < n; ++i) {
        if (m_archive.getTimeSampling(i) == ts) {
            return i;
        }
    }
    return 0;
}


int aiContext::getUid() const
{
    return m_uid;
}

const aiConfig& aiContext::getConfig() const
{
    return m_config;
}

void aiContext::setConfig(const aiConfig &config)
{
    m_config = config;
}

void aiContext::gatherNodesRecursive(aiObject *n)
{
    auto& abc = n->getAbcObject();
    size_t num_children = abc.getNumChildren();

    for (size_t i = 0; i < num_children; ++i) {
        auto *child = n->newChild(abc.getChild(i));
        gatherNodesRecursive(child);
    }
}

void aiContext::reset()
{
    waitAsync();
    m_top_node.reset();
    m_timesamplings.clear();
    m_archive.reset();

    m_path.clear();
    for (auto s : m_streams) { delete s; }
    m_streams.clear();

    // m_config is not reset intentionally
}

bool aiContext::load(const char *in_path)
{
    auto path = NormalizePath(in_path);
    auto wpath = L(in_path);

    DebugLogW(L"aiContext::load: '%s'", wpath.c_str());
    if (path == m_path && m_archive) {
        DebugLog("Context already loaded for gameObject with id %d", m_uid);
        return true;
    }

    reset();
    if (path.empty()) {
        return false;
    }

    m_path = path;
    if (!m_archive.valid()) {
        try {
            // Abc::IArchive doesn't accept wide string path. so create file stream with wide string path and pass it.
            // (VisualC++'s std::ifstream accepts wide string)
            m_streams.push_back(
#ifdef WIN32
                new lockFreeIStream(wpath.c_str())//std::ifstream(wpath.c_str(), std::ios::in | std::ios::binary)
#elif __linux__
                new std::ifstream(in_path, std::ios::in | std::ios::binary)
#else
                new std::ifstream(path.c_str(), std::ios::in | std::ios::binary)
#endif
            );

            Alembic::AbcCoreOgawa::ReadArchive archive_reader(m_streams);
            m_archive = Abc::IArchive(archive_reader(m_path), Abc::kWrapExisting, Abc::ErrorHandler::kThrowPolicy);
            DebugLog("Successfully opened Ogawa archive");
        }
        catch (Alembic::Util::Exception e) {
            // HDF5 archive doesn't accept external stream. so close it.
            // (that means if path contains wide characters, it can't be opened. I couldn't find solution..)
            for (auto s : m_streams) { delete s; }
            m_streams.clear();

            try {
                m_archive = Abc::IArchive(AbcCoreHDF5::ReadArchive(), path);
                DebugLog("Successfully opened HDF5 archive");
            }
            catch (Alembic::Util::Exception e2) {
                auto message = L(e2.what());
                DebugLogW(L"Failed to open archive: %s", message.c_str());
            }
        }
    }
    else {
        DebugLogW(L"Archive '%s' already opened", wpath.c_str());
    }

    if (m_archive.valid()) {
        abcObject abc_top = m_archive.getTop();
        m_top_node.reset(new aiObject(this, nullptr, abc_top));
        gatherNodesRecursive(m_top_node.get());

        m_timesamplings.clear();
        auto num_time_samplings = (int)m_archive.getNumTimeSamplings();
        for (int i = 0; i < num_time_samplings; ++i) {
            m_timesamplings.emplace_back(aiCreateTimeSampling(m_archive, i));
        }
        return true;
    }
    else {
        reset();
        return false;
    }
}

aiObject* aiContext::getTopObject() const
{
    return m_top_node.get();
}

void aiContext::updateSamples(double time)
{
    waitAsync();

    auto ss = aiTimeToSampleSelector(time);
    eachNodes([ss](aiObject& o) {
        o.updateSample(ss);
    });

    // kick async tasks!
    if (!m_async_tasks.empty()) {
        aiAsyncManager::instance().queue(m_async_tasks.data(), m_async_tasks.size());
    }
}

void aiContext::queueAsync(aiAsync& task)
{
    m_async_tasks.push_back(&task);
}

void aiContext::waitAsync()
{
    for (auto task : m_async_tasks)
        task->wait();
    m_async_tasks.clear();
}
