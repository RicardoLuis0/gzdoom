#pragma once
#include <stdint.h>
#include "tarray.h"
class DObject;
class FSerializer;

enum EObjectFlags
{
	// GC flags
	OF_White0			= 1 << 0,		// Object is white (type 0)
	OF_White1			= 1 << 1,		// Object is white (type 1)
	OF_Black			= 1 << 2,		// Object is black
	OF_Fixed			= 1 << 3,		// Object is fixed (should not be collected)
	OF_Rooted			= 1 << 4,		// Object is soft-rooted
	OF_EuthanizeMe		= 1 << 5,		// Object wants to die
	OF_Cleanup			= 1 << 6,		// Object is now being deleted by the collector
	OF_YesReallyDelete	= 1 << 7,		// Object is being deleted outside the collector, and this is okay, so don't print a warning

	OF_WhiteBits		= OF_White0 | OF_White1,
	OF_MarkBits			= OF_WhiteBits | OF_Black,

	// Other flags
	OF_JustSpawned		= 1 << 8,		// Thinker was spawned this tic
	OF_SerialSuccess	= 1 << 9,		// For debugging Serialize() calls
	OF_Sentinel			= 1 << 10,		// Object is serving as the sentinel in a ring list
	OF_Transient		= 1 << 11,		// Object should not be archived (references to it will be nulled on disk)
	OF_Spawned			= 1 << 12,      // Thinker was spawned at all (some thinkers get deleted before spawning)
	OF_Released			= 1 << 13,		// Object was released from the GC system and should not be processed by GC function
	OF_Networked		= 1 << 14,		// Object has a unique network identifier that makes it synchronizable between all clients.
	OF_ClientSide		= 1 << 15,		// Object is owned by a specific client rather than the server
};

template<class T> class TObjPtr;

namespace GC
{
	enum EGCState
	{
		GCS_Pause,
		GCS_Propagate,
		GCS_Sweep,
		GCS_Destroy,
		GCS_Done,

		GCS_COUNT
	};

	// Number of bytes currently allocated through M_Malloc/M_Realloc.
	extern size_t AllocBytes;

	// Number of bytes allocated since last collection step.
	extern size_t RunningAllocBytes;

	// Number of bytes freed since last collection step.
	extern size_t RunningDeallocBytes;

	// Amount of memory to allocate before triggering a collection.
	extern size_t Threshold;

	// List of gray objects.
	extern DObject *Gray;

	// List of every object.
	extern DObject *Root;

	// Current white value for potentially-live objects.
	extern uint32_t CurrentWhite;

	// Current collector state.
	extern EGCState State;

	// Position of GC sweep in the list of objects.
	extern DObject **SweepPos;

	// Size of GC pause.
	extern int Pause;

	// Size of GC steps.
	extern int StepMul;

	// Is this the final collection just before exit?
	extern bool FinalGC;

	// Current white value for known-dead objects.
	static inline uint32_t OtherWhite()
	{
		return CurrentWhite ^ OF_WhiteBits;
	}

	// Does one collection step.
	void Step();

	// Does a complete collection.
	void FullGC();

	// Handles the grunt work for a write barrier.
	void Barrier(DObject *pointing, DObject *pointed);

	// Handles a write barrier.
	static inline void WriteBarrier(DObject *pointing, DObject *pointed);

	// Handles a write barrier for a pointer that isn't inside an object.
	static inline void WriteBarrier(DObject *pointed);

	// Handles a read barrier.
	template<class T> inline T *ReadBarrier(T *&obj)
	{
		if (obj == NULL || !(obj->ObjectFlags & OF_EuthanizeMe))
		{
			return obj;
		}
		return obj = NULL;
	}

	// Handles a read barrier for a const pointer. This does not alter the source data, but only returns NULL if the object is destroyed.
	template<class T> inline T* ReadBarrier(const T*& obj)
	{
		if (obj == NULL || !(obj->ObjectFlags & OF_EuthanizeMe))
		{
			return obj;
		}
		return NULL;
	}

	// Check if it's time to collect, and do a collection step if it is.
	void CheckGC();

	// Forces a collection to start now.
	static inline void StartCollection()
	{
		Threshold = AllocBytes;
	}

	// Marks a white object gray. If the object wants to die, the pointer
	// is NULLed instead.
	void Mark(DObject **obj);

	// Marks an array of objects.
	void MarkArray(DObject **objs, size_t count);

	// For cleanup
	void DelSoftRootHead();

	// Soft-roots an object.
	void AddSoftRoot(DObject *obj);

	// Unroots an object.
	void DelSoftRoot(DObject *obj);

	template<class T> void Mark(T *&obj)
	{
		union
		{
			T *t;
			DObject *o;
		};
		o = obj;
		Mark(&o);
		obj = t;
	}
	template<class T> void Mark(TObjPtr<T> &obj);

	template<class T> void MarkArray(T **obj, size_t count)
	{
		MarkArray((DObject **)(obj), count);
	}
	template<class T> void MarkArray(TObjPtr<T>* obj, size_t count)
	{
		MarkArray((DObject**)(obj), count);
	}
	template<class T> void MarkArray(TArray<T> &arr)
	{
		MarkArray(&arr[0], arr.Size());
	}

	using GCMarkerFunc = void(*)();
	void AddMarkerFunc(GCMarkerFunc func);

	// Report an allocation to the GC
	static inline void ReportAlloc(size_t alloc)
	{
		AllocBytes += alloc;
		RunningAllocBytes += alloc;
	}

	// Report a deallocation to the GC
	static inline void ReportDealloc(size_t dealloc)
	{
		AllocBytes -= dealloc;
		RunningDeallocBytes += dealloc;
	}

	// Report a reallocation to the GC
	static inline void ReportRealloc(size_t oldsize, size_t newsize)
	{
		if (oldsize < newsize)
		{
			ReportAlloc(newsize - oldsize);
		}
		else
		{
			ReportDealloc(oldsize - newsize);
		}
	}
}

// A template class to help with handling read barriers. It does not
// handle write barriers, because those can be handled more efficiently
// with knowledge of the object that holds the pointer.
template<class T>
class TObjPtr
{
	union
	{
		mutable T pp;
		mutable DObject *o;
	};
public:

	constexpr TObjPtr<T>& operator=(T q) noexcept
	{
		pp = q;
		return *this;
	}

	constexpr TObjPtr<T>& operator=(std::nullptr_t nul) noexcept
	{
		o = nullptr;
		return *this;
	}

	// To allow NULL, too.
	TObjPtr<T>& operator=(const int val) noexcept
	{
		assert(val == 0);
		o = nullptr;
		return *this;
	}

	// To allow NULL, too. In Clang NULL is a long.
	TObjPtr<T>& operator=(const long val) noexcept
	{
		assert(val == 0);
		o = nullptr;
		return *this;
	}

	constexpr T Get() noexcept
	{
		return GC::ReadBarrier(pp);
	}

	constexpr T Get() const noexcept
	{
		auto ppp = pp;
		return GC::ReadBarrier(ppp);
	}

	constexpr T ForceGet() const noexcept	//for situations where the read barrier needs to be skipped.
	{
		return pp;
	}

	constexpr operator T() noexcept
	{
		return GC::ReadBarrier(pp);
	}
	constexpr T &operator*() noexcept
	{
		T q = GC::ReadBarrier(pp);
		assert(q != NULL);
		return *q;
	}
	constexpr T operator->() noexcept
	{
		return GC::ReadBarrier(pp);
	}
	
	constexpr const T operator->() const noexcept
	{
		return GC::ReadBarrier(pp);
	}

	constexpr bool operator!=(T u) const noexcept
	{
		return GC::ReadBarrier(o) != u;
	}
	constexpr bool operator==(T u) const noexcept
	{
		return GC::ReadBarrier(o) == u;
	}

	constexpr bool operator==(TObjPtr<T> u) const noexcept
	{
		return ForceGet() == u.ForceGet();
	}

	template<class U> friend inline void GC::Mark(TObjPtr<U> &obj);
	template<class U> friend FSerializer &Serialize(FSerializer &arc, const char *key, TObjPtr<U> &value, TObjPtr<U> *);
	template<class U> friend FSerializer &Serialize(FSerializer &arc, const char *key, TObjPtr<U> &value, U *);

	friend class DObject;
};

// This is only needed because some parts of GCC do not treat a class with any constructor as trivial.
// TObjPtr needs to be fully trivial, though - some parts in the engine depend on it.
template<class T>
constexpr TObjPtr<T> MakeObjPtr(T t) noexcept
{
	// since this exists to replace the constructor we cannot initialize in the declaration as this would require the constructor we want to avoid.
	TObjPtr<T> tt;
	tt = t;
	return tt;
}

// Use barrier_cast instead of static_cast when you need to cast
// the contents of a TObjPtr to a related type.
template<class T,class U> inline T barrier_cast(TObjPtr<U> &o)
{
	return static_cast<T>(static_cast<U>(o));
}

namespace GC
{
	template<class T> inline void Mark(TObjPtr<T> &obj)
	{
		GC::Mark(&obj.o);
	}
}
