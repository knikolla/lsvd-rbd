/*
 * file:        nvme.cc
 * description: implementation of read/write requests to local SSD
 * author:      Peter Desnoyers, Northeastern University
 * Copyright 2021, 2022 Peter Desnoyers
 * license:     GNU LGPL v2.1 or newer
 *              LGPL-2.1-or-later
 */

#include <libaio.h>
#include <signal.h>
#include <sys/uio.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include "backend.h"
#include "extent.h"
#include "io.h"
#include "lsvd_types.h"
#include "misc_cache.h"
#include "request.h"
#include "smartiov.h"
// #include "utils.h"
#include <liburing.h>

#include "nvme.h"

void do_log(const char *, ...);

class nvme_aio;

class nvme_aio : public nvme
{
  public:
    int fd;
    bool e_io_running = false;
    std::thread e_io_th;
    io_context_t ioctx;

    static void sighandler(int sig) { pthread_exit(NULL); }

    nvme_aio(int fd_, const char *name_)
    {
        fd = fd_;
        e_io_running = true;
        io_queue_init(64, &ioctx);
        signal(SIGUSR2, sighandler);
        e_io_th = std::thread(e_iocb_runner, ioctx, &e_io_running, name_);
    }

    ~nvme_aio()
    {
        e_io_running = false;
        // e_io_th.join();
        // pthread_cancel(e_io_th.native_handle());
        pthread_kill(e_io_th.native_handle(), SIGUSR2);
        e_io_th.join();
        io_queue_release(ioctx);
    }

    int read(void *buf, size_t count, off_t offset)
    {
        return pread(fd, buf, count, offset);
    }

    int write(const void *buf, size_t count, off_t offset)
    {
        return pwrite(fd, buf, count, offset);
    }

    int writev(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return pwritev(fd, iov, iovcnt, offset);
    }

    int readv(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return preadv(fd, iov, iovcnt, offset);
    }

    request *make_write_request(smartiov *iov, size_t offset)
    {
        assert(offset != 0);
        auto req = new nvme_aio_request(iov, offset, OP_WRITE, this);
        return (request *)req;
    }

    request *make_write_request(char *buf, size_t len, size_t offset)
    {
        assert(offset != 0);
        auto req = new nvme_aio_request(buf, len, offset, OP_WRITE, this);
        return (request *)req;
    }

    request *make_read_request(smartiov *iov, size_t offset)
    {
        auto req = new nvme_aio_request(iov, offset, OP_READ, this);
        return (request *)req;
    }

    request *make_read_request(char *buf, size_t len, size_t offset)
    {
        auto req = new nvme_aio_request(buf, len, offset, OP_READ, this);
        return (request *)req;
    }

    static void call_send_request_notify(void *parent, long res)
    {
        nvme_aio_request *r = (nvme_aio_request *)parent;
        r->notify2(res);
    }

  private:
    class nvme_aio_request : public request
    {
      public:
        e_iocb eio;
        smartiov _iovs;
        size_t ofs;
        enum lsvd_op op;
        nvme_aio *nvme_ptr;
        request *parent;

        bool released = false;
        bool complete = false;
        std::mutex m;
        std::condition_variable cv;

      public:
        nvme_aio_request(smartiov *iov, size_t offset, enum lsvd_op type,
                         nvme_aio *nvme_w)
        {
            _iovs.ingest(iov->data(), iov->size());
            ofs = offset;
            op = type;
            nvme_ptr = nvme_w;
        }

        nvme_aio_request(char *buf, size_t len, size_t offset,
                         enum lsvd_op type, nvme_aio *nvme_w)
        {
            _iovs.push_back((iovec){buf, len});
            ofs = offset;
            op = type;
            nvme_ptr = nvme_w;
        }

        ~nvme_aio_request() {}

        void wait()
        {
            std::unique_lock lk(m);
            while (!complete)
                cv.wait(lk);
        }

        void run(request *parent_)
        {
            parent = parent_;
            if (op == OP_WRITE) {
                e_io_prep_pwritev(&eio, nvme_ptr->fd, _iovs.data(),
                                  _iovs.size(), ofs,
                                  nvme_aio::call_send_request_notify, this);
            } else
                e_io_prep_preadv(&eio, nvme_ptr->fd, _iovs.data(), _iovs.size(),
                                 ofs, nvme_aio::call_send_request_notify, this);
            e_io_submit(nvme_ptr->ioctx, &eio);
        }

        void notify(request *child) {}

        void notify2(long result)
        {
            if (parent)
                parent->notify(this);
            assert((size_t)result == _iovs.bytes());

            std::unique_lock lk(m);
            complete = true;
            cv.notify_one();
            if (released) {
                lk.unlock();
                delete this;
            }
        }

        void release()
        {
            std::unique_lock lk(m);
            released = true;
            if (complete) { // TODO: atomic swap?
                lk.unlock();
                delete this;
            }
        }
    };
};

class nvme_uring : public nvme
{
  private:
    io_uring ring;
    std::thread uring_cqe_worker;

    std::atomic_bool cqe_worker_should_continue = true;

  public:
    int fd;

    nvme_uring(int fd_, const char *name_)
    {
        fd = fd_;
        auto ioret = io_uring_queue_init(64, &ring, 0);
        assert(ioret == 0);
        // debug("io_uring queue initialised");

        // TEMPORARY UGLY HACK, copied from running io_uring-cp under gdb
        // ring.sq.ring_mask = 63;
        // ring.sq.ring_entries = 64;
        // ring.cq.ring_mask = 127;
        // ring.cq.ring_entries = 128;

        uring_cqe_worker =
            std::thread(&nvme_uring::uring_completion_worker, this);
    }

    ~nvme_uring() { io_uring_queue_exit(&ring); }

    int read(void *buf, size_t count, off_t offset)
    {
        return pread(fd, buf, count, offset);
    }

    int write(const void *buf, size_t count, off_t offset)
    {
        return pwrite(fd, buf, count, offset);
    }

    int writev(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return pwritev(fd, iov, iovcnt, offset);
    }

    int readv(const struct iovec *iov, int iovcnt, off_t offset)
    {
        return preadv(fd, iov, iovcnt, offset);
    }

    request *make_write_request(smartiov *iov, size_t offset)
    {
        assert(offset != 0);
        return (request *)new nvme_uring_request(iov, offset, OP_WRITE, this);
    }

    request *make_write_request(char *buf, size_t len, size_t offset)
    {
        assert(offset != 0);
        return (request *)new nvme_uring_request(buf, len, offset, OP_WRITE,
                                                 this);
    }

    request *make_read_request(smartiov *iov, size_t offset)
    {
        return (request *)new nvme_uring_request(iov, offset, OP_READ, this);
    }

    request *make_read_request(char *buf, size_t len, size_t offset)
    {
        return (request *)new nvme_uring_request(buf, len, offset, OP_READ,
                                                 this);
    }

    void uring_completion_worker()
    {
        __kernel_timespec timeout = {0, 1000 * 100}; // 100 microseconds
        while (cqe_worker_should_continue.load()) {
            io_uring_cqe *cqe;
            // auto res = io_uring_wait_cqe_timeout(&ring, &cqe, &timeout);
            auto res = io_uring_wait_cqe(&ring, &cqe);

            // TODO handle error
            if (res != 0)
                continue;

            nvme_uring_request *req =
                static_cast<nvme_uring_request *>(io_uring_cqe_get_data(cqe));

            if (cqe->res < 0)
                req->on_fail(-1 * cqe->res);
            else
                req->on_complete(cqe->res);

            io_uring_cqe_seen(&ring, cqe);
        }
    }

  private:
    class nvme_uring_request : public request
    {
      private:
        nvme_uring *nvmu_;
        lsvd_op op_;
        smartiov iovs_;
        size_t offset_;

        // other bookkeeping, copied from the aio version
        request *parent_;
        bool released_ = false;
        bool complete_ = false;
        std::mutex completion_mtx;
        std::condition_variable cv;

      public:
        nvme_uring_request(smartiov *iovs, size_t offset, lsvd_op op,
                           nvme_uring *nvmu)
            : nvmu_(nvmu), op_(op), offset_(offset)
        {
            iovs_.ingest(iovs->data(), iovs->size());
        }

        nvme_uring_request(char *buf, size_t len, size_t offset, lsvd_op op,
                           nvme_uring *nvmu)
            : nvmu_(nvmu), op_(op), offset_(offset)
        {
            iovs_.push_back((iovec){buf, len});
        }

        void run(request *parent)
        {
            // debug("submitting uring request, type {}, len {}", op_,
                //   iovs_.bytes());
            // debug("setting parent to {}", parent);
            parent_ = parent;
            io_uring_sqe *sqe = io_uring_get_sqe(&nvmu_->ring);
            assert(sqe != NULL);

            // TODO prep_write instead of writev if iov has only 1 element
            if (op_ == OP_WRITE)
                io_uring_prep_writev(sqe, nvmu_->fd, iovs_.data(), iovs_.size(),
                                     offset_);
            else if (op_ == OP_READ)
                io_uring_prep_readv(sqe, nvmu_->fd, iovs_.data(), iovs_.size(),
                                    offset_);
            else
                assert(false);

            io_uring_sqe_set_data(sqe, this);
            io_uring_submit(&nvmu_->ring);
        }

        void on_complete(int result)
        {
            if (parent_)
                parent_->notify(this);

            // if op was successful this should always be true
            assert((size_t)result == iovs_.bytes());

            // directly copied from the aio version
            std::unique_lock lk(completion_mtx);
            complete_ = true;
            cv.notify_all();
            if (released_) {
                lk.unlock();
                delete this;
            }
        }

        void on_fail(int errnum)
        {
            // TODO there's no failure path yet so just no-op
            return;
        }

        void notify(request *child)
        {
            // no-op; the cqe worker calls on_complete directly
            return;
        }

        void release()
        {
            // directly copied from the aio version
            std::unique_lock lk(completion_mtx);
            released_ = true;
            if (complete_) { // TODO: atomic swap?
                lk.unlock();
                delete this;
            }
        }

        void wait()
        {
            // directly copied from the aio version
            std::unique_lock lk(completion_mtx);
            while (!complete_)
                cv.wait(lk);
        }
    };
};

nvme *make_nvme_aio(int fd, const char *name)
{
    return (nvme *)new nvme_aio(fd, name);
}

nvme *make_nvme_uring(int fd, const char *name)
{
    return (nvme *)new nvme_uring(fd, name);
}
