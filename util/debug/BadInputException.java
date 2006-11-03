public class BadInputException extends Exception
{
	public final long offset;
	
	public BadInputException(long offset)
	{
		super();
		this.offset = offset;
	}
	
	public BadInputException(String message, long offset)
	{
		super(message);
		this.offset = offset;
	}
	
	public BadInputException(Throwable cause, long offset)
	{
		super(cause);
		this.offset = offset;
	}
	
	public BadInputException(String message, Throwable cause, long offset)
	{
		super(message, cause);
		this.offset = offset;
	}
}
