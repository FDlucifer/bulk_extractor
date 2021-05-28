#include <random>
#include <chrono>
#include <thread>

#include "config.h"
#include "bulk_extractor.h"
#include "phase1.h"
#include "be13_api/utils.h"


/**
 * Implementation of the bulk_extractor Phase 1.
 *
 * bulk_extractor 1.0:
 * - run() creates worker threads.
 *   -  main thread stuffs the work queue with sbufs to process.
 *   -  workers remove each sbuf and process with each scanner.
 *   -  recursive work is processed within each thread.
 *
 * bulk_extractor 2.0:
 * - implements 1.0 mechanism.
 * - implements 2.0 mechanism, which uses a work unit for each sbuf/scanner combination.
 */

using namespace std::chrono_literals;

/**
 * convert tsec into a string.
 */

std::string BulkExtractor_Phase1::minsec(time_t tsec)
{
    time_t min = tsec / 60;
    time_t sec = tsec % 60;
    std::stringstream ss;
    if (min>0) ss << min << " min";
    if (sec>0) ss << sec << " sec";
    return ss.str();
}

/*
 * Print the status of each thread in the threadpool.
 */
void BulkExtractor_Phase1::print_tp_status()
{
    for(u_int i=0;i<config.num_threads;i++){
#if 0
        std::string status = tp->get_thread_status(i);
        if (status.size() && status!="Free"){
            std::cout << "Thread " << i << ": " << status << "\n";
        }
#endif
    }
    std::cout << "\n";
}

/**
 * attempt to get an sbuf. If we can't get it, we may be in a
 * low-memory situation.  wait for 30 seconds.
 *
 * TODO: do not get an sbuf if more than N tasks in workqueue.
 */

sbuf_t *BulkExtractor_Phase1::get_sbuf(image_process::iterator &it)
{
    for(u_int retry_count=0;retry_count<config.max_bad_alloc_errors;retry_count++){
        try {
            return it.sbuf_alloc(); // may throw exception
        }
        catch (const std::bad_alloc &e) {
            // Low memory could come from a bad sbuf alloc or another low memory condition.
            // wait for a while and then try again...
            std::cerr << "Low Memory (bad_alloc) exception: " << e.what()
                      << " reading " << it.get_pos0()
                      << " (retry_count=" << retry_count
                      << " of " << config.max_bad_alloc_errors << ")\n";

            std::stringstream str;
            str << "name='bad_alloc' " << "pos0='" << it.get_pos0() << "' " << "retry_count='"     << retry_count << "' ";
            xreport.xmlout("debug:exception", e.what(), str.str(), true);
        }
        if (retry_count < config.max_bad_alloc_errors+1){
            std::cerr << "will wait for " << config.retry_seconds << " seconds and try again...\n";
            std::this_thread::sleep_for(config.retry_seconds * 1000ms);
        }
    }
    std::cerr << "Too many errors encountered in a row. Diagnose and restart.\n";
    throw std::runtime_error("too many sbuf allocation errors");
}


void BulkExtractor_Phase1::notify_user(image_process::iterator &it)
{
    if (notify_ctr++ >= config.opt_notify_rate){
        time_t t = time(0);
        struct tm tm;
        localtime_r(&t,&tm);
        printf("%2d:%02d:%02d %s ",tm.tm_hour,tm.tm_min,tm.tm_sec,it.str().c_str());

        /* not sure how to do the rest if sampling */
        if (!sampling()){
            printf("(%4.2f%%) Done in %s at %s",
                   it.fraction_done()*100.0,
                   timer.eta_text(it.fraction_done()).c_str(),
                   timer.eta_time(it.fraction_done()).c_str());
        }
        printf("\n");
        fflush(stdout);
        notify_ctr = 0;
    }
}

/**
 * Create a list sorted list of random blocks.
 */
void BulkExtractor_Phase1::make_sorted_random_blocklist(blocklist_t *blocklist,uint64_t max_blocks,float frac)
{
    if (frac>0.2){
        std::cerr << "A better random sampler is needed for this many random blocks. \n"
                  << "We should have an iterator that decides if to include or not to include based on frac.\n";
        throw std::runtime_error("Specify frac<0.2");
    }

    std::default_random_engine generator;
    std::uniform_int_distribution<uint64_t> distribution(0,max_blocks);
    while(blocklist->size() < max_blocks * frac){
        blocklist->insert( distribution(generator) ); // will be added even if already present
    }
}


void BulkExtractor_Phase1::set_sampling_parameters(Config &c,std::string &p)
{
    std::vector<std::string> params = split(p,':');
    if (params.size()!=1 && params.size()!=2){
        throw std::runtime_error("sampling parameters must be fraction[:passes]");
    }
    c.sampling_fraction = atof(params.at(0).c_str());
    if (c.sampling_fraction<=0 || c.sampling_fraction>=1){
        throw std::runtime_error("error: sampling fraction f must be 0<f<=1");
    }
    if (params.size()==2){
        c.sampling_passes = atoi(params.at(1).c_str());
        if (c.sampling_passes==0){
            throw std::runtime_error("error: sampling passes must be >=1");
        }
    }
}

struct  work_unit {
    work_unit(scanner_set &ss_,sbuf_t *sbuf_):ss(ss_),sbuf(sbuf_){}
    scanner_set &ss;
    sbuf_t *sbuf;
    void process() const {
        ss.process_sbuf(sbuf);
    }
};

void BulkExtractor_Phase1::send_data_to_workers()
{
    xreport.push("runtime","xmlns:debug=\"http://www.github.com/simsong/bulk_extractor/issues\"");

    /* A single loop with two iterators.
     *
     * it -- the regular image_iterator; it knows how to read blocks.
     *
     * si -- the sampling iterator. It is a iterator for an STL set.
     *
     * If sampling, si is used to ask for a specific page from it.
     */
    blocklist_t blocks_to_sample;
    blocklist_t::const_iterator si = blocks_to_sample.begin(); // sampling iterator
    image_process::iterator     it = p.begin(); // sequential iterator

    if (config.opt_offset_start){
        std::cout << "offset set to " << config.opt_offset_start << "\n";
        it.set_raw_offset(config.opt_offset_start);
    }

    if (sampling()){
        /* Create a list of blocks to sample */
        make_sorted_random_blocklist(&blocks_to_sample,it.max_blocks(),config.sampling_fraction);
        si = blocks_to_sample.begin();    // get the new beginning
    }
    /* Loop over the blocks to sample */
    while(true){
        if (sampling()){
            if (si==blocks_to_sample.end()) break;
            it.seek_block(*si);
        } else {
            /* Not sampling; no need to seek, it's the next one */
            if (it == p.end()){
                break;
            }
        }

        if (config.opt_offset_end!=0 && config.opt_offset_end <= it.raw_offset ){
            break;                      // passed the offset
        }

        if (config.opt_page_start<=it.page_number && config.opt_offset_start<=it.raw_offset){
            // Make sure we haven't done this page yet
            if (seen_page_ids.find(it.get_pos0().str()) == seen_page_ids.end()){
                try {
                    sbuf_t *sbufp = get_sbuf(it);
                    auto   &sbuf = *sbufp;

                    /* compute the sha1 hash */
                    if (sha1g){
                        if (sbuf.pos0.offset==sha1_next){
                            // next byte follows logically, so continue to compute hash
                            sha1g->update(sbuf.buf, sbuf.pagesize);
                            sha1_next += sbuf.pagesize;
                        } else {
                            delete sha1g; // we had a logical gap; stop hashing
                            sha1g = 0;
                        }
                    }
                    total_bytes += sbuf.pagesize;

                    /***************************
                     **** SCHEDULE THE WORK ****
                     ***************************/

                    struct work_unit wu(ss, sbufp);
                    tp->push( [wu]{ wu.process(); } );
                    if (config.opt_quiet == false ){
                        notify_user(it);
                    }
                }
                catch (const std::exception &e) {
                    // report uncaught exceptions to both user and XML file
                    std::stringstream sstr;
                    sstr << "name='" << e.what() << "' " << "pos0='" << it.get_pos0() << "' ";

                    if (config.opt_report_read_errors) {
                        std::cerr << "Exception " << e.what() << " skipping " << it.get_pos0() << "\n";
                    }
                    xreport.xmlout("debug:exception", e.what(), sstr.str(), true);
                }
            }
        } // end that we haven't seen it

        /* If we are random sampling, move to the next random sample.
         * Otherwise increment the it iterator.
         */
        if (sampling()){
            ++si;
        } else {
            ++it;
        }
    }

    if (!config.opt_quiet){
        std::cout << "All data are read; waiting for threads to finish...\n";
    }
}


void BulkExtractor_Phase1::wait_for_workers()
{
    /* Now wait for all of the threads to be free */
    //tp->mode = 1;			// waiting for workers to finish
    time_t wait_start = time(0);
    for(int32_t counter = 0;;counter++){
        //int num_remaining = config.num_threads - tp->get_free_count();
        //if (num_remaining==0) break;

        std::this_thread::sleep_for(100ms);
        time_t time_waiting   = time(0) - wait_start;
        time_t time_remaining = config.max_wait_time - time_waiting;

        if (counter%60==0){
            std::stringstream sstr;
            sstr << "Time elapsed waiting for "
                // << num_remaining
                // << " thread" << (num_remaining>1 ? "s" : "")
               << " to finish:\n    " << minsec(time_waiting)
               << " (timeout in "     << minsec(time_remaining) << ".)\n";
            if (config.opt_quiet==0){
                std::cout << sstr.str();
                if (counter>0) print_tp_status();
            }
            xreport.comment(sstr.str());
        }
        if (time_waiting>config.max_wait_time){
            std::cout << "\n\n";
            std::cout << " ... this shouldn't take more than an hour. Exiting ... \n";
            std::cout << " ... Please report to the bulk_extractor maintainer ... \n";
            break;
        }
    }
    if (config.opt_quiet==0) std::cout << "All Threads Finished!\n";

    xreport.pop();			// pop runtime
    /* We can write out the source info now, since we (might) know the hash */
    xreport.push("source");
    xreport.xmlout("image_filename",p.image_fname());
    xreport.xmlout("image_size",p.image_size());
    if (sha1g){
        dfxml::sha1_t sha1 = sha1g->digest();
        //if (sha1_string) *sha1_string = sha1.hexdigest();
        xreport.xmlout("hashdigest",sha1.hexdigest(),"type='SHA1'",false);
        delete sha1g;
    }
    xreport.pop();			// source

    /* Record the feature files and their counts in the output */
    //tp->fs.dump_name_count_stats(xreport);

    //if (config.opt_quiet==0) std::cout << "Producer time spent waiting: " << tp->waiting.elapsed_seconds() << " sec.\n";

    //xreport.xmlout("thread_wait",dtos(tp->waiting.elapsed_seconds()),"thread='0'",false);
    double worker_wait_average = 0;
#if 0
    for(threadpool::worker_vector::const_iterator ij=tp->workers.begin();ij!=tp->workers.end();ij++){
        worker_wait_average += (*ij)->waiting.elapsed_seconds() / config.num_threads;
        std::stringstream sstr;
        sstr << "thread='" << (*ij)->id << "'";
        xreport.xmlout("thread_wait",dtos((*ij)->waiting.elapsed_seconds()),sstr.str(),false);
    }
#endif
    xreport.pop();
    xreport.flush();
    if (config.opt_quiet==0) std::cout << "Average consumer time spent waiting: " << worker_wait_average << " sec.\n";
#if 0
    if (worker_wait_average > tp->waiting.elapsed_seconds()*2
       && worker_wait_average>10 && config.opt_quiet==0){
        std::cout << "*******************************************\n";
        std::cout << "** bulk_extractor is probably I/O bound. **\n";
        std::cout << "**        Run with a faster drive        **\n";
        std::cout << "**      to get better performance.       **\n";
        std::cout << "*******************************************\n";
    }
    if (tp->waiting.elapsed_seconds() > worker_wait_average * 2
       && tp->waiting.elapsed_seconds()>10 && config.opt_quiet==0){
        std::cout << "*******************************************\n";
        std::cout << "** bulk_extractor is probably CPU bound. **\n";
        std::cout << "**    Run on a computer with more cores  **\n";
        std::cout << "**      to get better performance.       **\n";
        std::cout << "*******************************************\n";
    }
#endif
    /* end of phase 1 */
}

void BulkExtractor_Phase1::run()
{
    /* Create the threadpool and launch the workers */
    p.set_report_read_errors(config.opt_report_read_errors);
    tp = new threadpool(config.num_threads); // ,fs,xreport);

    send_data_to_workers();
    std::cerr << "calling join...\n";
    tp->join();
    //wait_for_workers();
    std::cerr << "done!\n";
}
