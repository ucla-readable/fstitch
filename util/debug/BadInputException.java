public class BadInputException extends Exception
{
	public final int offset;
	
	public BadInputException(int offset)
	{
		super();
		this.offset = offset;
	}
	
	public BadInputException(String message, int offset)
	{
		super(message);
		this.offset = offset;
	}
	
	public BadInputException(Throwable cause, int offset)
	{
		super(cause);
		this.offset = offset;
	}
	
	public BadInputException(String message, Throwable cause, int offset)
	{
		super(message, cause);
		this.offset = offset;
	}
}
