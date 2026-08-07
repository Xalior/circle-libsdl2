/*
 * sdl-app-init.ld — the constructor arrays, split into "before the kernel"
 * and "after it".
 *
 * INCLUDE THIS from an application's own linker script, inside SECTIONS,
 * where the .init_array output section would otherwise go:
 *
 *     SECTIONS {
 *         ...
 *         INCLUDE sdl-app-init.ld
 *         ...
 *     }
 *
 * It is found through the -L path that sdl-app.mk adds, so an application
 * that links the ordinary way needs nothing else. sdl-app.ld includes it
 * too, so an application that has not overridden the script gets it without
 * knowing it exists.
 *
 * ---------------------------------------------------------------------------
 * WHY THE ARRAY IS SPLIT AT ALL
 *
 * Circle's sysinit runs .init_array BEFORE main, and therefore before CKernel
 * is constructed — so before CTimer, CDeviceNameService, CScheduler and
 * CLogger exist. Every Circle singleton's Get() asserts that its instance is
 * present, and the assertion handler reports through CLogger::Get(), which is
 * equally absent, and then spins forever.
 *
 * So a static constructor in an APPLICATION that touches any of it stops the
 * board dead, before a single character has reached the wire. It is not a
 * crash that can be read: there is no output, no fault, and nothing to
 * suggest where to look. One real example, since the mechanism is easier to
 * believe with one: a constructor seeding a random generator from
 * std::chrono::system_clock::now() reaches clock_gettime, which reaches
 * CTimer::Get(), which asserts.
 *
 * Applications cannot reasonably be asked to avoid this — the constructor is
 * usually in third-party code, and the reach to a Circle singleton is several
 * calls deep through the C library. So the constructors run LATER instead,
 * once the kernel exists, and the runtime half of this lives in the library
 * (see SDL2Circle_RunDeferredConstructors).
 *
 * ---------------------------------------------------------------------------
 * WHERE THE LINE IS DRAWN, AND WHY IT IS DRAWN FROM THIS SIDE
 *
 * The obvious split is to name the application's own objects and defer those.
 * That cannot live in a library: it would mean this file knowing the
 * directory names of every application that ever links against it.
 *
 * The other side of the same line IS knowable, and it is a property of how a
 * program is built rather than of what it is called: THE FRAMEWORK ARRIVES AS
 * ARCHIVES AND THE APPLICATION ARRIVES AS OBJECTS. The shim, Circle, libc++,
 * libunwind and the C library are all `.a` files on the link line; the
 * application's own translation units are `.o` files. So archive members run
 * early, plain objects run late, and no name from either side appears here.
 *
 * The C runtime's own start objects are the exception that has to be named:
 * crtbegin and crtend are objects rather than archive members, and they carry
 * the frame-registration constructors that everything else depends on.
 *
 * An application's HOST objects are objects too, so they defer with the rest.
 * That is correct rather than merely tolerable: a host constructor that
 * touches Circle would fail in exactly the same way, and deferring it is the
 * fix rather than a side effect.
 *
 * ---------------------------------------------------------------------------
 * PRIORITISED CONSTRUCTORS STAY EARLY, AND STAY SORTED
 *
 * .init_array.NNNNN carries an explicit init_priority, and the sort is what
 * makes the priority mean anything. A plain KEEP(*(.init_array*)) — which is
 * what this replaced — matches the prioritised sections too and emits them in
 * link order, silently discarding the ordering the priority asked for. In one
 * application that put libc++'s iostream initialiser behind thirty-eight
 * application constructors. Nothing used a stream that early, so it did not
 * bite; it was still wrong.
 *
 * They are kept early as well as sorted. A priority is a statement that
 * something must run before ordinary constructors, and the ordinary
 * constructors of the framework run here.
 */

	.init_array : {
		__init_start = .;

		/* Prioritised, sorted, whoever they belong to. */
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))

		/* The C runtime's start objects: named because they are objects
		 * rather than archive members. */
		KEEP(*crtbegin*.o(.init_array))
		KEEP(*crtend*.o(.init_array))
		KEEP(*crti*.o(.init_array))
		KEEP(*crtn*.o(.init_array))

		/* The framework: every archive member, whatever the archive. */
		KEEP(*.a:*(.init_array))

		__init_end = .;

		/* Everything left is the application, and runs once the kernel
		 * exists. These two symbols are what the library walks; they are
		 * always defined, so an empty range simply means there was
		 * nothing to defer. */
		__sdl2_deferred_init_start = .;

		KEEP(*(.init_array))

		__sdl2_deferred_init_end = .;
	}
