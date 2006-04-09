import java.util.HashMap;
import java.util.Iterator;
import java.util.NoSuchElementException;

public class ChdescCollection
{
	private final HashMap chdescs;
	private final String dupError;
	int size;
	
	private class CollectionElement
	{
		public final Chdesc chdesc;
		public int count;
		
		public CollectionElement(Chdesc chdesc)
		{
			this.chdesc = chdesc;
			count = 1;
		}
	}
	
	private class CollectionIterator implements Iterator
	{
		private HashMapValueIterator iterator;
		private CollectionElement element;
		private int count;
		
		public CollectionIterator()
		{
			iterator = new HashMapValueIterator(chdescs);
			advance();
		}
		
		private void advance()
		{
			if(iterator.hasNext())
				element = (CollectionElement) iterator.next();
			else
				element = null;
			count = 0;
		}
		
		public boolean hasNext()
		{
			return element != null;
		}
		
		public Object next()
		{
			if(element == null)
				throw new NoSuchElementException();
			Chdesc chdesc = element.chdesc;
			if(++count == element.count)
				advance();
			return chdesc;
		}
		
		public void remove()
		{
			throw new UnsupportedOperationException();
		}
	}
	
	public ChdescCollection()
	{
		chdescs = new HashMap();
		dupError = null;
		size = 0;
	}
	
	public ChdescCollection(String dupError)
	{
		chdescs = new HashMap();
		this.dupError = dupError;
		size = 0;
	}
	
	public void add(Chdesc chdesc)
	{
		//Integer key = Integer.valueOf(chdesc.address);
		Integer key = new Integer(chdesc.address);
		CollectionElement element = (CollectionElement) chdescs.get(key);
		if(element != null)
		{
			if(dupError != null)
				throw new RuntimeException("Duplicate chdesc " + dupError + "!");;
			element.count++;
		}
		else
			chdescs.put(key, new CollectionElement(chdesc));
		size++;
	}
	
	public Chdesc lookup(int chdesc)
	{
		//Integer key = Integer.valueOf(chdesc);
		Integer key = new Integer(chdesc);
		CollectionElement element = (CollectionElement) chdescs.get(key);
		return (element != null) ? element.chdesc : null;
	}
	
	public Chdesc remove(int chdesc)
	{
		//Integer key = Integer.valueOf(chdesc);
		Integer key = new Integer(chdesc);
		CollectionElement element = (CollectionElement) chdescs.get(key);
		if(element == null)
			return null;
		if(--element.count == 0)
			chdescs.remove(key);
		size--;
		return element.chdesc;
	}
	
	public int size()
	{
		return size;
	}
	
	public Iterator iterator()
	{
		return new CollectionIterator();
	}
}
